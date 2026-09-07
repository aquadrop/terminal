// Copyright (c) Microsoft Corporation.
// Licensed under the MIT license.

#define NOMINMAX
#define WIN32_LEAN_AND_MEAN

#include <Windows.h>
#include <ShObjIdl.h>
#include <shellapi.h>
#include <wrl/client.h>

#include <algorithm>
#include <cstddef>
#include <cstring>
#include <filesystem>
#include <fstream>
#include <iostream>
#include <optional>
#include <regex>
#include <set>
#include <string>
#include <string_view>
#include <system_error>
#include <vector>

#include "CoreFileCommands.h"
#include "PathTranslation.h"

using Microsoft::WRL::ComPtr;

namespace
{
    constexpr int usageError = 2;

    struct ParsedFileOptions
    {
        bool recursive = false;
        bool force = false;
        bool noClobber = false;
        std::vector<std::filesystem::path> operands;
    };

    struct GrepOptions
    {
        bool ignoreCase = false;
        bool lineNumber = false;
        bool invertMatch = false;
        bool fixedStrings = false;
        bool extendedRegex = false;
        bool count = false;
        bool filesWithMatches = false;
        bool recursive = false;
        bool followDirectorySymlinks = false;
        bool withFilename = false;
        bool noFilename = false;
        std::vector<std::wstring> patterns;
        std::vector<std::filesystem::path> paths;
    };

    struct GrepResult
    {
        bool selected = false;
        bool error = false;
    };

    class RecycleOnlyProgressSink final : public IFileOperationProgressSink
    {
    public:
        HRESULT STDMETHODCALLTYPE QueryInterface(const IID& iid, void** result) override
        {
            if (!result)
            {
                return E_POINTER;
            }

            *result = nullptr;
            if (iid == IID_IUnknown || iid == IID_IFileOperationProgressSink)
            {
                *result = static_cast<IFileOperationProgressSink*>(this);
                AddRef();
                return S_OK;
            }
            return E_NOINTERFACE;
        }

        ULONG STDMETHODCALLTYPE AddRef() override
        {
            return static_cast<ULONG>(InterlockedIncrement(&_references));
        }

        ULONG STDMETHODCALLTYPE Release() override
        {
            const auto references = InterlockedDecrement(&_references);
            if (references == 0)
            {
                delete this;
            }
            return static_cast<ULONG>(references);
        }

        HRESULT STDMETHODCALLTYPE StartOperations() override
        {
            return S_OK;
        }

        HRESULT STDMETHODCALLTYPE FinishOperations(HRESULT) override
        {
            return S_OK;
        }

        HRESULT STDMETHODCALLTYPE PreRenameItem(DWORD, IShellItem*, PCWSTR) override
        {
            return S_OK;
        }

        HRESULT STDMETHODCALLTYPE PostRenameItem(DWORD, IShellItem*, PCWSTR, HRESULT, IShellItem*) override
        {
            return S_OK;
        }

        HRESULT STDMETHODCALLTYPE PreMoveItem(DWORD, IShellItem*, IShellItem*, PCWSTR) override
        {
            return S_OK;
        }

        HRESULT STDMETHODCALLTYPE PostMoveItem(DWORD, IShellItem*, IShellItem*, PCWSTR, HRESULT, IShellItem*) override
        {
            return S_OK;
        }

        HRESULT STDMETHODCALLTYPE PreCopyItem(DWORD, IShellItem*, IShellItem*, PCWSTR) override
        {
            return S_OK;
        }

        HRESULT STDMETHODCALLTYPE PostCopyItem(DWORD, IShellItem*, IShellItem*, PCWSTR, HRESULT, IShellItem*) override
        {
            return S_OK;
        }

        HRESULT STDMETHODCALLTYPE PreDeleteItem(const DWORD flags, IShellItem*) override
        {
            if ((flags & TSF_DELETE_RECYCLE_IF_POSSIBLE) == 0)
            {
                _recycleRejected = true;
                return E_ABORT;
            }
            return S_OK;
        }

        HRESULT STDMETHODCALLTYPE PostDeleteItem(DWORD,
                                                 IShellItem*,
                                                 const HRESULT deleteResult,
                                                 IShellItem* recycledItem) override
        {
            if (SUCCEEDED(deleteResult) && !recycledItem)
            {
                _permanentDeleteObserved = true;
                return E_ABORT;
            }
            return S_OK;
        }

        HRESULT STDMETHODCALLTYPE PreNewItem(DWORD, IShellItem*, PCWSTR) override
        {
            return S_OK;
        }

        HRESULT STDMETHODCALLTYPE PostNewItem(DWORD, IShellItem*, PCWSTR, PCWSTR, DWORD, HRESULT, IShellItem*) override
        {
            return S_OK;
        }

        HRESULT STDMETHODCALLTYPE UpdateProgress(UINT, UINT) override
        {
            return S_OK;
        }

        HRESULT STDMETHODCALLTYPE ResetTimer() override
        {
            return S_OK;
        }

        HRESULT STDMETHODCALLTYPE PauseTimer() override
        {
            return S_OK;
        }

        HRESULT STDMETHODCALLTYPE ResumeTimer() override
        {
            return S_OK;
        }

        bool RecycleRejected() const noexcept
        {
            return _recycleRejected;
        }

        bool PermanentDeleteObserved() const noexcept
        {
            return _permanentDeleteObserved;
        }

    private:
        ~RecycleOnlyProgressSink() = default;

        LONG _references = 1;
        bool _recycleRejected = false;
        bool _permanentDeleteObserved = false;
    };

    class CoInitializeScope
    {
    public:
        CoInitializeScope() noexcept :
            _result{ CoInitializeEx(nullptr, COINIT_APARTMENTTHREADED | COINIT_DISABLE_OLE1DDE) }
        {
        }

        ~CoInitializeScope()
        {
            if (SUCCEEDED(_result))
            {
                CoUninitialize();
            }
        }

        HRESULT Result() const noexcept
        {
            return _result;
        }

    private:
        HRESULT _result;
    };

    class UniqueHandle
    {
    public:
        explicit UniqueHandle(const HANDLE value = INVALID_HANDLE_VALUE) noexcept :
            _value{ value }
        {
        }

        ~UniqueHandle()
        {
            if (_value != INVALID_HANDLE_VALUE)
            {
                CloseHandle(_value);
            }
        }

        UniqueHandle(const UniqueHandle&) = delete;
        UniqueHandle& operator=(const UniqueHandle&) = delete;

        HANDLE Get() const noexcept
        {
            return _value;
        }

        explicit operator bool() const noexcept
        {
            return _value != INVALID_HANDLE_VALUE;
        }

    private:
        HANDLE _value;
    };

    std::wstring asciiLower(std::wstring value)
    {
        for (auto& ch : value)
        {
            if (ch >= L'A' && ch <= L'Z')
            {
                ch += L'a' - L'A';
            }
        }
        return value;
    }

    std::filesystem::path withoutTrailingSeparators(const std::filesystem::path& path)
    {
        auto value = path.native();
        while (!value.empty() && (value.back() == L'\\' || value.back() == L'/'))
        {
            const std::filesystem::path candidate{ value };
            if (candidate == candidate.root_path())
            {
                break;
            }
            value.pop_back();
        }
        return std::filesystem::path{ value };
    }

    bool pathLessInsensitive(const std::filesystem::path& left,
                             const std::filesystem::path& right)
    {
        const auto& leftValue = left.native();
        const auto& rightValue = right.native();
        const auto comparison = CompareStringOrdinal(leftValue.data(),
                                                     static_cast<int>(leftValue.size()),
                                                     rightValue.data(),
                                                     static_cast<int>(rightValue.size()),
                                                     TRUE);
        return comparison == CSTR_LESS_THAN ||
               (comparison == 0 && leftValue < rightValue);
    }

    bool pathComponentEquals(const std::filesystem::path& left,
                             const std::filesystem::path& right)
    {
        const auto& leftValue = left.native();
        const auto& rightValue = right.native();
        return CompareStringOrdinal(leftValue.data(),
                                    static_cast<int>(leftValue.size()),
                                    rightValue.data(),
                                    static_cast<int>(rightValue.size()),
                                    TRUE) == CSTR_EQUAL;
    }

    std::optional<std::filesystem::path> volumePathFor(const std::filesystem::path& path)
    {
        std::error_code error;
        const auto absolute = std::filesystem::absolute(path, error);
        if (error)
        {
            return std::nullopt;
        }

        std::wstring volumePath(32768, L'\0');
        if (!GetVolumePathNameW(absolute.c_str(),
                                volumePath.data(),
                                static_cast<DWORD>(volumePath.size())))
        {
            return std::nullopt;
        }
        volumePath.resize(wcslen(volumePath.c_str()));
        return std::filesystem::path{ volumePath };
    }

    bool targetIsSourceOrDescendant(const std::filesystem::path& source,
                                    const std::filesystem::path& target,
                                    std::error_code& error)
    {
        const auto canonicalSource = std::filesystem::weakly_canonical(source, error);
        if (error)
        {
            return false;
        }
        const auto canonicalTarget = std::filesystem::weakly_canonical(target, error);
        if (error)
        {
            return false;
        }

        auto sourcePart = canonicalSource.begin();
        auto targetPart = canonicalTarget.begin();
        for (; sourcePart != canonicalSource.end(); ++sourcePart, ++targetPart)
        {
            if (targetPart == canonicalTarget.end() || !pathComponentEquals(*sourcePart, *targetPart))
            {
                return false;
            }
        }
        return true;
    }

    std::wstring widen(const std::string_view value, const UINT codePage)
    {
        if (value.empty())
        {
            return {};
        }

        const auto required = MultiByteToWideChar(codePage, 0, value.data(), static_cast<int>(value.size()), nullptr, 0);
        if (required <= 0)
        {
            return L"unknown error";
        }

        std::wstring result(static_cast<size_t>(required), L'\0');
        MultiByteToWideChar(codePage, 0, value.data(), static_cast<int>(value.size()), result.data(), required);
        return result;
    }

    std::optional<std::string> toUtf8(const std::wstring_view value)
    {
        if (value.empty())
        {
            return std::string{};
        }

        const auto required = WideCharToMultiByte(CP_UTF8,
                                                  WC_ERR_INVALID_CHARS,
                                                  value.data(),
                                                  static_cast<int>(value.size()),
                                                  nullptr,
                                                  0,
                                                  nullptr,
                                                  nullptr);
        if (required <= 0)
        {
            return std::nullopt;
        }

        std::string result(static_cast<size_t>(required), '\0');
        if (WideCharToMultiByte(CP_UTF8,
                                WC_ERR_INVALID_CHARS,
                                value.data(),
                                static_cast<int>(value.size()),
                                result.data(),
                                required,
                                nullptr,
                                nullptr) <= 0)
        {
            return std::nullopt;
        }
        return result;
    }

    std::string pathToUtf8(const std::filesystem::path& path)
    {
        const auto value = path.u8string();
        return { reinterpret_cast<const char*>(value.data()), value.size() };
    }

    void reportError(const std::wstring_view command,
                     const std::filesystem::path& path,
                     const std::wstring_view message)
    {
        std::wcerr << command << L": ";
        if (!path.empty())
        {
            std::wcerr << path.native() << L": ";
        }
        std::wcerr << message << L'\n';
    }

    void reportError(const std::wstring_view command,
                     const std::filesystem::path& path,
                     const std::error_code& error)
    {
        reportError(command, path, widen(error.message(), CP_ACP));
    }

    void reportWin32Error(const std::wstring_view command,
                          const std::filesystem::path& path,
                          const DWORD error)
    {
        std::error_code code{ static_cast<int>(error), std::system_category() };
        reportError(command, path, code);
    }

    void printFileUsage(const std::wstring_view command)
    {
        if (command == L"cp")
        {
            std::wcout << L"usage: cp [-Rr] [-f|-n] SOURCE... DESTINATION\n";
        }
        else if (command == L"mv")
        {
            std::wcout << L"usage: mv [-f|-n] SOURCE... DESTINATION\n";
        }
        else
        {
            std::wcout << L"usage: rm [-Rrf] FILE...\n";
        }
    }

    int launchTerminal()
    {
        std::wstring modulePath(32768, L'\0');
        const auto length = GetModuleFileNameW(nullptr,
                                               modulePath.data(),
                                               static_cast<DWORD>(modulePath.size()));
        if (length == 0 || length == modulePath.size())
        {
            reportWin32Error(L"wt", {}, GetLastError());
            return 1;
        }
        modulePath.resize(length);

        std::filesystem::path executable{ modulePath };
        executable.replace_filename(L"WindowsTerminal.exe");

        std::wstring commandLine{ GetCommandLineW() };
        STARTUPINFOW startupInfo{};
        startupInfo.cb = sizeof(startupInfo);
        GetStartupInfoW(&startupInfo);

        PROCESS_INFORMATION processInfo{};
        if (!CreateProcessW(executable.c_str(),
                            commandLine.data(),
                            nullptr,
                            nullptr,
                            FALSE,
                            0,
                            nullptr,
                            nullptr,
                            &startupInfo,
                            &processInfo))
        {
            reportWin32Error(L"wt", executable, GetLastError());
            return 1;
        }

        CloseHandle(processInfo.hThread);
        CloseHandle(processInfo.hProcess);
        return 0;
    }

    bool parseFileOptions(const std::wstring_view command,
                          const std::vector<std::wstring_view>& arguments,
                          ParsedFileOptions& options)
    {
        bool optionsEnded = false;
        for (const auto argument : arguments)
        {
            if (!optionsEnded && argument == L"--")
            {
                optionsEnded = true;
                continue;
            }
            if (!optionsEnded && argument == L"--help")
            {
                printFileUsage(command);
                return false;
            }
            if (!optionsEnded && argument.starts_with(L"--"))
            {
                if ((command == L"cp" || command == L"rm") && argument == L"--recursive")
                {
                    options.recursive = true;
                }
                else if (argument == L"--force")
                {
                    options.force = true;
                    options.noClobber = false;
                }
                else if ((command == L"cp" || command == L"mv") && argument == L"--no-clobber")
                {
                    options.noClobber = true;
                    options.force = false;
                }
                else
                {
                    reportError(command, {}, L"unsupported option " + std::wstring{ argument });
                    return false;
                }
                continue;
            }
            if (!optionsEnded && argument.size() > 1 && argument.front() == L'-')
            {
                for (const auto option : argument.substr(1))
                {
                    if ((command == L"cp" || command == L"rm") && (option == L'r' || option == L'R'))
                    {
                        options.recursive = true;
                    }
                    else if (option == L'f')
                    {
                        options.force = true;
                        options.noClobber = false;
                    }
                    else if ((command == L"cp" || command == L"mv") && option == L'n')
                    {
                        options.noClobber = true;
                        options.force = false;
                    }
                    else
                    {
                        reportError(command, {}, std::wstring{ L"unsupported option -" } + option);
                        return false;
                    }
                }
                continue;
            }

            options.operands.emplace_back(Terminal::UnixCommandShim::ConvertLinuxPath(argument));
        }
        return true;
    }

    bool validateSourceAndDestination(const std::wstring_view command,
                                      const ParsedFileOptions& options,
                                      std::filesystem::path& destination,
                                      bool& destinationIsDirectory)
    {
        if (options.operands.size() < 2)
        {
            printFileUsage(command);
            return false;
        }

        destination = options.operands.back();
        std::error_code error;
        const auto destinationStatus = std::filesystem::status(destination, error);
        if (destinationStatus.type() == std::filesystem::file_type::not_found)
        {
            error.clear();
            destinationIsDirectory = false;
        }
        else if (error)
        {
            reportError(command, destination, error);
            return false;
        }
        else
        {
            destinationIsDirectory = std::filesystem::is_directory(destinationStatus);
        }
        if (options.operands.size() > 2 && !destinationIsDirectory)
        {
            reportError(command, destination, L"destination must be a directory when using multiple sources");
            return false;
        }
        return true;
    }

    bool createTemporaryPath(const std::filesystem::path& target,
                             std::filesystem::path& temporary,
                             std::error_code& error)
    {
        GUID identifier{};
        if (FAILED(CoCreateGuid(&identifier)))
        {
            error = std::make_error_code(std::errc::io_error);
            return false;
        }

        wchar_t identifierText[39]{};
        if (StringFromGUID2(identifier, identifierText, static_cast<int>(std::size(identifierText))) == 0)
        {
            error = std::make_error_code(std::errc::io_error);
            return false;
        }

        auto temporaryName = target.native();
        temporaryName.append(L".wtcmd-");
        temporaryName.append(identifierText);
        temporaryName.append(L".tmp");
        temporary = std::filesystem::path{ temporaryName };
        error.clear();
        return true;
    }

    bool replaceWithTemporary(const std::filesystem::path& temporary,
                              const std::filesystem::path& target,
                              std::error_code& error)
    {
        const auto attributes = GetFileAttributesW(target.c_str());
        const auto readOnly = attributes != INVALID_FILE_ATTRIBUTES &&
                              (attributes & FILE_ATTRIBUTE_READONLY) != 0;
        if (readOnly && !SetFileAttributesW(target.c_str(), attributes & ~FILE_ATTRIBUTE_READONLY))
        {
            error = { static_cast<int>(GetLastError()), std::system_category() };
        }
        else if (!MoveFileExW(temporary.c_str(),
                              target.c_str(),
                              MOVEFILE_REPLACE_EXISTING | MOVEFILE_WRITE_THROUGH))
        {
            error = { static_cast<int>(GetLastError()), std::system_category() };
            if (readOnly)
            {
                SetFileAttributesW(target.c_str(), attributes);
            }
        }
        else
        {
            error.clear();
        }

        if (error)
        {
            std::error_code cleanupError;
            std::filesystem::remove(temporary, cleanupError);
            return false;
        }
        return true;
    }

    bool filesHaveSameContents(const HANDLE source,
                               const HANDLE target,
                               std::error_code& error)
    {
        LARGE_INTEGER sourceSize{};
        LARGE_INTEGER targetSize{};
        if (!GetFileSizeEx(source, &sourceSize) || !GetFileSizeEx(target, &targetSize))
        {
            error = { static_cast<int>(GetLastError()), std::system_category() };
            return false;
        }
        if (sourceSize.QuadPart != targetSize.QuadPart)
        {
            error = std::make_error_code(std::errc::io_error);
            return false;
        }

        LARGE_INTEGER start{};
        if (!SetFilePointerEx(source, start, nullptr, FILE_BEGIN) ||
            !SetFilePointerEx(target, start, nullptr, FILE_BEGIN))
        {
            error = { static_cast<int>(GetLastError()), std::system_category() };
            return false;
        }

        std::vector<char> sourceBuffer(1024 * 1024);
        std::vector<char> targetBuffer(sourceBuffer.size());
        for (;;)
        {
            DWORD sourceRead = 0;
            DWORD targetRead = 0;
            if (!ReadFile(source,
                          sourceBuffer.data(),
                          static_cast<DWORD>(sourceBuffer.size()),
                          &sourceRead,
                          nullptr) ||
                !ReadFile(target,
                          targetBuffer.data(),
                          static_cast<DWORD>(targetBuffer.size()),
                          &targetRead,
                          nullptr))
            {
                error = { static_cast<int>(GetLastError()), std::system_category() };
                return false;
            }
            if (sourceRead != targetRead ||
                std::memcmp(sourceBuffer.data(), targetBuffer.data(), sourceRead) != 0)
            {
                error = std::make_error_code(std::errc::io_error);
                return false;
            }
            if (sourceRead == 0)
            {
                error.clear();
                return true;
            }
        }
    }

    bool discardTemporaryHandle(const HANDLE handle)
    {
        FILE_BASIC_INFO basicInformation{};
        if (!GetFileInformationByHandleEx(handle,
                                          FileBasicInfo,
                                          &basicInformation,
                                          sizeof(basicInformation)))
        {
            return false;
        }
        if ((basicInformation.FileAttributes & FILE_ATTRIBUTE_READONLY) != 0)
        {
            basicInformation.FileAttributes &= ~FILE_ATTRIBUTE_READONLY;
            if (!SetFileInformationByHandle(handle,
                                            FileBasicInfo,
                                            &basicInformation,
                                            sizeof(basicInformation)))
            {
                return false;
            }
        }

        FILE_DISPOSITION_INFO disposition{ TRUE };
        return SetFileInformationByHandle(handle,
                                          FileDispositionInfo,
                                          &disposition,
                                          sizeof(disposition)) != FALSE;
    }

    bool discardTemporaryPath(const std::filesystem::path& path,
                              std::error_code& error)
    {
        const UniqueHandle handle{ CreateFileW(path.c_str(),
                                               DELETE | FILE_READ_ATTRIBUTES | FILE_WRITE_ATTRIBUTES,
                                               FILE_SHARE_READ,
                                               nullptr,
                                               OPEN_EXISTING,
                                               FILE_FLAG_OPEN_REPARSE_POINT,
                                               nullptr) };
        if (!handle)
        {
            const auto openError = GetLastError();
            if (openError == ERROR_FILE_NOT_FOUND || openError == ERROR_PATH_NOT_FOUND)
            {
                error.clear();
                return true;
            }
            error = { static_cast<int>(openError), std::system_category() };
            return false;
        }

        BY_HANDLE_FILE_INFORMATION information{};
        if (!GetFileInformationByHandle(handle.Get(), &information) ||
            (information.dwFileAttributes & FILE_ATTRIBUTE_REPARSE_POINT) != 0 ||
            information.nNumberOfLinks != 1)
        {
            error = std::make_error_code(std::errc::operation_not_permitted);
            return false;
        }

        if (!discardTemporaryHandle(handle.Get()))
        {
            error = { static_cast<int>(GetLastError()), std::system_category() };
            return false;
        }
        error.clear();
        return true;
    }

    bool forceCopyFile(const std::filesystem::path& source,
                       const std::filesystem::path& target,
                       std::error_code& error)
    {
        std::filesystem::path temporary;
        if (!createTemporaryPath(target, temporary, error))
        {
            return false;
        }

        if (!CopyFileW(source.c_str(), temporary.c_str(), TRUE))
        {
            const auto copyError = GetLastError();
            if (copyError != ERROR_FILE_EXISTS && copyError != ERROR_ALREADY_EXISTS)
            {
                std::error_code cleanupError;
                if (!discardTemporaryPath(temporary, cleanupError))
                {
                    error = cleanupError;
                    return false;
                }
            }
            error = { static_cast<int>(copyError), std::system_category() };
            return false;
        }

        const UniqueHandle temporaryHandle{ CreateFileW(temporary.c_str(),
                                                        GENERIC_READ | DELETE | FILE_WRITE_ATTRIBUTES,
                                                        FILE_SHARE_READ,
                                                        nullptr,
                                                        OPEN_EXISTING,
                                                        FILE_FLAG_OPEN_REPARSE_POINT | FILE_FLAG_SEQUENTIAL_SCAN,
                                                        nullptr) };
        if (!temporaryHandle)
        {
            const auto openError = GetLastError();
            if (!discardTemporaryPath(temporary, error))
            {
                return false;
            }
            error = { static_cast<int>(openError), std::system_category() };
            return false;
        }

        const auto discardTemporary = [&]() {
            if (!discardTemporaryHandle(temporaryHandle.Get()))
            {
                error = { static_cast<int>(GetLastError()), std::system_category() };
            }
        };

        BY_HANDLE_FILE_INFORMATION fileInformation{};
        if (!GetFileInformationByHandle(temporaryHandle.Get(), &fileInformation))
        {
            error = { static_cast<int>(GetLastError()), std::system_category() };
            discardTemporary();
            return false;
        }
        if ((fileInformation.dwFileAttributes & FILE_ATTRIBUTE_REPARSE_POINT) != 0 ||
            fileInformation.nNumberOfLinks != 1)
        {
            error = std::make_error_code(std::errc::operation_not_permitted);
            discardTemporary();
            return false;
        }

        const UniqueHandle sourceHandle{ CreateFileW(source.c_str(),
                                                     GENERIC_READ,
                                                     FILE_SHARE_READ | FILE_SHARE_WRITE | FILE_SHARE_DELETE,
                                                     nullptr,
                                                     OPEN_EXISTING,
                                                     FILE_FLAG_SEQUENTIAL_SCAN,
                                                     nullptr) };
        if (!sourceHandle)
        {
            error = { static_cast<int>(GetLastError()), std::system_category() };
            discardTemporary();
            return false;
        }
        if (!filesHaveSameContents(sourceHandle.Get(), temporaryHandle.Get(), error))
        {
            discardTemporary();
            return false;
        }

        const auto absoluteTarget = std::filesystem::absolute(target, error);
        if (error)
        {
            discardTemporary();
            return false;
        }

        const auto targetName = absoluteTarget.native();
        const auto renameSize = offsetof(FILE_RENAME_INFO, FileName) +
                                (targetName.size() + 1) * sizeof(wchar_t);
        std::vector<std::byte> renameBuffer(renameSize);
        auto* rename = reinterpret_cast<FILE_RENAME_INFO*>(renameBuffer.data());
        rename->ReplaceIfExists = TRUE;
        rename->RootDirectory = nullptr;
        const auto targetNameBytes = targetName.size() * sizeof(wchar_t);
        rename->FileNameLength = static_cast<DWORD>(targetNameBytes);
        std::memcpy(rename->FileName, targetName.data(), targetNameBytes);
        rename->FileName[targetName.size()] = L'\0';

        const auto attributes = GetFileAttributesW(target.c_str());
        const auto readOnly = attributes != INVALID_FILE_ATTRIBUTES &&
                              (attributes & FILE_ATTRIBUTE_READONLY) != 0;
        if (readOnly && !SetFileAttributesW(target.c_str(), attributes & ~FILE_ATTRIBUTE_READONLY))
        {
            error = { static_cast<int>(GetLastError()), std::system_category() };
            discardTemporary();
            return false;
        }

        if (!SetFileInformationByHandle(temporaryHandle.Get(),
                                        FileRenameInfo,
                                        rename,
                                        static_cast<DWORD>(renameBuffer.size())))
        {
            error = { static_cast<int>(GetLastError()), std::system_category() };
            if (readOnly)
            {
                SetFileAttributesW(target.c_str(), attributes);
            }
            discardTemporary();
            return false;
        }

        error.clear();
        return true;
    }

    bool copySymbolicLink(const std::filesystem::path& source,
                          const std::filesystem::path& target,
                          const ParsedFileOptions& options,
                          std::error_code& error)
    {
        const auto targetStatus = std::filesystem::symlink_status(target, error);
        const auto targetExists = targetStatus.type() != std::filesystem::file_type::not_found;
        if (!targetExists)
        {
            error.clear();
            std::filesystem::copy_symlink(source, target, error);
            return !error;
        }
        if (error)
        {
            return false;
        }
        if (options.noClobber)
        {
            return true;
        }

        std::filesystem::path temporary;
        if (!createTemporaryPath(target, temporary, error))
        {
            return false;
        }
        std::filesystem::copy_symlink(source, temporary, error);
        if (error)
        {
            std::error_code cleanupError;
            std::filesystem::remove(temporary, cleanupError);
            return false;
        }
        return replaceWithTemporary(temporary, target, error);
    }

    bool copyRegularFile(const std::filesystem::path& source,
                         const std::filesystem::path& target,
                         const ParsedFileOptions& options,
                         std::error_code& error)
    {
        const auto targetStatus = std::filesystem::symlink_status(target, error);
        const auto targetExists = targetStatus.type() != std::filesystem::file_type::not_found;
        if (!targetExists)
        {
            error.clear();
        }
        else if (error)
        {
            return false;
        }

        if (targetExists && options.noClobber)
        {
            return true;
        }
        if (targetExists && options.force && !std::filesystem::is_directory(targetStatus))
        {
            return forceCopyFile(source, target, error);
        }

        const auto copyOptions = options.noClobber ?
                                     std::filesystem::copy_options::skip_existing :
                                     std::filesystem::copy_options::overwrite_existing;
        std::filesystem::copy_file(source, target, copyOptions, error);
        return !error;
    }

    bool forceCopyDirectory(const std::filesystem::path& source,
                            const std::filesystem::path& target,
                            const ParsedFileOptions& options,
                            std::error_code& error)
    {
        std::filesystem::create_directories(target, error);
        if (error)
        {
            return false;
        }

        std::filesystem::recursive_directory_iterator iterator{ source, error };
        const std::filesystem::recursive_directory_iterator end;
        if (error)
        {
            return false;
        }

        while (iterator != end)
        {
            const auto entry = *iterator;
            const auto relative = entry.path().lexically_relative(source);
            if (relative.empty())
            {
                error = std::make_error_code(std::errc::invalid_argument);
                return false;
            }
            const auto entryTarget = target / relative;
            const auto status = entry.symlink_status(error);
            if (error)
            {
                return false;
            }
            const auto attributes = GetFileAttributesW(entry.path().c_str());
            if (attributes != INVALID_FILE_ATTRIBUTES &&
                (attributes & FILE_ATTRIBUTE_REPARSE_POINT) != 0)
            {
                iterator.disable_recursion_pending();
                if (!std::filesystem::is_symlink(status))
                {
                    error = std::make_error_code(std::errc::operation_not_supported);
                    return false;
                }
            }

            if (std::filesystem::is_directory(status))
            {
                std::filesystem::create_directories(entryTarget, error);
            }
            else if (std::filesystem::is_regular_file(status))
            {
                copyRegularFile(entry.path(), entryTarget, options, error);
            }
            else if (std::filesystem::is_symlink(status))
            {
                copySymbolicLink(entry.path(), entryTarget, options, error);
            }
            else
            {
                std::filesystem::copy(entry.path(),
                                      entryTarget,
                                      std::filesystem::copy_options::copy_symlinks |
                                          std::filesystem::copy_options::overwrite_existing,
                                      error);
            }
            if (error)
            {
                return false;
            }

            iterator.increment(error);
            if (error)
            {
                return false;
            }
        }
        return true;
    }

    int copyCommand(const std::vector<std::wstring_view>& arguments)
    {
        ParsedFileOptions options;
        if (!parseFileOptions(L"cp", arguments, options))
        {
            return arguments.size() == 1 && arguments.front() == L"--help" ? 0 : usageError;
        }

        std::filesystem::path destination;
        bool destinationIsDirectory = false;
        if (!validateSourceAndDestination(L"cp", options, destination, destinationIsDirectory))
        {
            return usageError;
        }

        auto result = 0;
        for (size_t index = 0; index + 1 < options.operands.size(); ++index)
        {
            const auto& source = options.operands[index];
            std::error_code error;
            const auto status = std::filesystem::status(source, error);
            if (error || status.type() == std::filesystem::file_type::not_found)
            {
                reportError(L"cp", source, error ? error : std::make_error_code(std::errc::no_such_file_or_directory));
                result = 1;
                continue;
            }
            const auto linkStatus = std::filesystem::symlink_status(source, error);
            if (error)
            {
                reportError(L"cp", source, error);
                result = 1;
                continue;
            }
            const auto sourceIsSymbolicLink = std::filesystem::is_symlink(linkStatus);
            const auto sourceAttributes = GetFileAttributesW(source.c_str());
            if (sourceAttributes != INVALID_FILE_ATTRIBUTES &&
                (sourceAttributes & FILE_ATTRIBUTE_REPARSE_POINT) != 0 &&
                !sourceIsSymbolicLink)
            {
                reportError(L"cp", source, L"unsupported directory reparse point");
                result = 1;
                continue;
            }

            const auto sourceName = withoutTrailingSeparators(source).filename();
            if (sourceName.empty() || sourceName == L"..")
            {
                reportError(L"cp", source, L"could not determine the source name");
                result = 1;
                continue;
            }

            const auto copyContents = sourceName == L".";
            auto target = destinationIsDirectory && !copyContents ? destination / sourceName : destination;
            const auto sourceIsDirectory = !sourceIsSymbolicLink && std::filesystem::is_directory(status);
            const auto targetStatus = std::filesystem::symlink_status(target, error);
            if (targetStatus.type() == std::filesystem::file_type::not_found)
            {
                error.clear();
            }
            else if (error)
            {
                reportError(L"cp", target, error);
                result = 1;
                continue;
            }
            else if (std::filesystem::equivalent(source, target, error))
            {
                reportError(L"cp", source, L"source and destination are the same file");
                result = 1;
                continue;
            }
            if (error)
            {
                reportError(L"cp", target, error);
                result = 1;
                continue;
            }
            if (sourceIsDirectory && !options.recursive)
            {
                reportError(L"cp", source, L"is a directory; use -r to copy it");
                result = 1;
                continue;
            }
            if (sourceIsDirectory && targetIsSourceOrDescendant(source, target, error))
            {
                reportError(L"cp", target, L"cannot copy a directory into itself");
                result = 1;
                continue;
            }
            if (error)
            {
                reportError(L"cp", target, error);
                result = 1;
                continue;
            }

            auto copyOptions = options.noClobber ?
                                   std::filesystem::copy_options::skip_existing :
                                   std::filesystem::copy_options::overwrite_existing;
            copyOptions |= std::filesystem::copy_options::copy_symlinks;
            if (options.recursive)
            {
                copyOptions |= std::filesystem::copy_options::recursive;
            }

            if (sourceIsSymbolicLink)
            {
                copySymbolicLink(source, target, options, error);
            }
            else if (sourceIsDirectory)
            {
                if (options.force)
                {
                    forceCopyDirectory(source, target, options, error);
                }
                else
                {
                    std::filesystem::copy(source, target, copyOptions, error);
                }
            }
            else
            {
                copyRegularFile(source, target, options, error);
            }

            if (error)
            {
                reportError(L"cp", source, error);
                result = 1;
            }
        }
        return result;
    }

    int moveCommand(const std::vector<std::wstring_view>& arguments)
    {
        ParsedFileOptions options;
        if (!parseFileOptions(L"mv", arguments, options))
        {
            return arguments.size() == 1 && arguments.front() == L"--help" ? 0 : usageError;
        }

        std::filesystem::path destination;
        bool destinationIsDirectory = false;
        if (!validateSourceAndDestination(L"mv", options, destination, destinationIsDirectory))
        {
            return usageError;
        }

        auto result = 0;
        for (size_t index = 0; index + 1 < options.operands.size(); ++index)
        {
            const auto& source = options.operands[index];
            const auto sourceName = withoutTrailingSeparators(source).filename();
            if (sourceName.empty() || sourceName == L"." || sourceName == L"..")
            {
                reportError(L"mv", source, L"could not determine the source name");
                result = 1;
                continue;
            }
            const auto target = destinationIsDirectory ? destination / sourceName : destination;

            std::error_code error;
            if (!std::filesystem::exists(source, error))
            {
                reportError(L"mv", source, error ? error : std::make_error_code(std::errc::no_such_file_or_directory));
                result = 1;
                continue;
            }
            if (error)
            {
                reportError(L"mv", source, error);
                result = 1;
                continue;
            }

            if (options.noClobber && std::filesystem::exists(target, error))
            {
                if (error)
                {
                    reportError(L"mv", target, error);
                    result = 1;
                }
                continue;
            }

            auto flags = MOVEFILE_COPY_ALLOWED | MOVEFILE_WRITE_THROUGH;
            if (!options.noClobber)
            {
                flags |= MOVEFILE_REPLACE_EXISTING;
            }
            const auto sourceVolume = volumePathFor(source);
            const auto targetVolume = volumePathFor(target);
            const auto crossVolume = sourceVolume &&
                                     targetVolume &&
                                     !pathComponentEquals(withoutTrailingSeparators(*sourceVolume),
                                                          withoutTrailingSeparators(*targetVolume));

            if (!MoveFileExW(source.c_str(), target.c_str(), flags))
            {
                reportWin32Error(L"mv", source, GetLastError());
                result = 1;
            }
            else if (crossVolume && GetFileAttributesW(source.c_str()) != INVALID_FILE_ATTRIBUTES)
            {
                reportError(L"mv", source, L"source still exists after the move");
                result = 1;
            }
            else if (crossVolume)
            {
                const auto lastError = GetLastError();
                if (lastError != ERROR_FILE_NOT_FOUND && lastError != ERROR_PATH_NOT_FOUND)
                {
                    reportWin32Error(L"mv", source, lastError);
                    result = 1;
                }
            }
        }
        return result;
    }

    int recycleCommand(const std::vector<std::wstring_view>& arguments)
    {
        ParsedFileOptions options;
        if (!parseFileOptions(L"rm", arguments, options))
        {
            return arguments.size() == 1 && arguments.front() == L"--help" ? 0 : usageError;
        }
        if (options.operands.empty())
        {
            if (options.force)
            {
                return 0;
            }
            printFileUsage(L"rm");
            return usageError;
        }

        CoInitializeScope initialize;
        if (FAILED(initialize.Result()))
        {
            reportWin32Error(L"rm", {}, static_cast<DWORD>(initialize.Result()));
            return 1;
        }

        ComPtr<IFileOperation> operation;
        auto result = CoCreateInstance(CLSID_FileOperation,
                                       nullptr,
                                       CLSCTX_INPROC_SERVER,
                                       IID_PPV_ARGS(operation.GetAddressOf()));
        if (FAILED(result))
        {
            reportWin32Error(L"rm", {}, static_cast<DWORD>(result));
            return 1;
        }

        constexpr DWORD operationFlags = FOF_NO_UI |
                                         FOF_NOERRORUI |
                                         FOF_SILENT |
                                         FOFX_ADDUNDORECORD |
                                         FOFX_EARLYFAILURE |
                                         FOFX_RECYCLEONDELETE;
        result = operation->SetOperationFlags(operationFlags);
        if (FAILED(result))
        {
            reportWin32Error(L"rm", {}, static_cast<DWORD>(result));
            return 1;
        }

        ComPtr<RecycleOnlyProgressSink> progressSink;
        progressSink.Attach(new RecycleOnlyProgressSink{});

        auto exitCode = 0;
        size_t queuedOperations = 0;
        for (const auto& operand : options.operands)
        {
            const auto lexicalOperand = withoutTrailingSeparators(operand);
            if (lexicalOperand.filename() == L"." || lexicalOperand.filename() == L"..")
            {
                reportError(L"rm", operand, L"refusing to remove '.' or '..'");
                exitCode = 1;
                continue;
            }

            std::error_code error;
            const auto absolute = std::filesystem::absolute(operand, error).lexically_normal();
            if (error)
            {
                reportError(L"rm", operand, error);
                exitCode = 1;
                continue;
            }
            std::wstring volumePath(32768, L'\0');
            if (!GetVolumePathNameW(absolute.c_str(),
                                    volumePath.data(),
                                    static_cast<DWORD>(volumePath.size())))
            {
                reportWin32Error(L"rm", operand, GetLastError());
                exitCode = 1;
                continue;
            }
            volumePath.resize(wcslen(volumePath.c_str()));
            if (pathComponentEquals(withoutTrailingSeparators(absolute),
                                    withoutTrailingSeparators(volumePath)))
            {
                reportError(L"rm", operand, L"refusing to remove a filesystem root");
                exitCode = 1;
                continue;
            }
            if (GetDriveTypeW(volumePath.c_str()) != DRIVE_FIXED)
            {
                reportError(L"rm", operand, L"the location does not provide a guaranteed local Recycle Bin");
                exitCode = 1;
                continue;
            }

            SHQUERYRBINFO recycleBinInfo{ sizeof(recycleBinInfo) };
            result = SHQueryRecycleBinW(volumePath.c_str(), &recycleBinInfo);
            if (FAILED(result))
            {
                reportError(L"rm", operand, L"the Recycle Bin is unavailable for this location");
                exitCode = 1;
                continue;
            }

            const auto attributes = GetFileAttributesW(absolute.c_str());
            if (attributes == INVALID_FILE_ATTRIBUTES)
            {
                const auto lastError = GetLastError();
                if (!options.force || (lastError != ERROR_FILE_NOT_FOUND && lastError != ERROR_PATH_NOT_FOUND))
                {
                    reportWin32Error(L"rm", operand, lastError);
                    exitCode = 1;
                }
                continue;
            }
            if ((attributes & FILE_ATTRIBUTE_DIRECTORY) != 0 && !options.recursive)
            {
                reportError(L"rm", operand, L"is a directory; use -r to recycle it");
                exitCode = 1;
                continue;
            }

            ComPtr<IShellItem> item;
            result = SHCreateItemFromParsingName(absolute.c_str(),
                                                 nullptr,
                                                 IID_PPV_ARGS(item.GetAddressOf()));
            if (FAILED(result))
            {
                reportWin32Error(L"rm", operand, static_cast<DWORD>(result));
                exitCode = 1;
                continue;
            }

            result = operation->DeleteItem(item.Get(), progressSink.Get());
            if (FAILED(result))
            {
                reportWin32Error(L"rm", operand, static_cast<DWORD>(result));
                exitCode = 1;
                continue;
            }
            ++queuedOperations;
        }

        if (queuedOperations == 0)
        {
            return exitCode;
        }

        result = operation->PerformOperations();
        if (progressSink->RecycleRejected())
        {
            reportError(L"rm", {}, L"the item cannot be moved to the Recycle Bin");
            return 1;
        }
        if (progressSink->PermanentDeleteObserved())
        {
            reportError(L"rm", {}, L"the Shell did not return a recycled item");
            return 1;
        }
        if (FAILED(result))
        {
            reportWin32Error(L"rm", {}, static_cast<DWORD>(result));
            return 1;
        }

        BOOL aborted = FALSE;
        result = operation->GetAnyOperationsAborted(&aborted);
        if (FAILED(result) || aborted)
        {
            if (FAILED(result))
            {
                reportWin32Error(L"rm", {}, static_cast<DWORD>(result));
            }
            else
            {
                reportError(L"rm", {}, L"Recycle Bin operation was aborted");
            }
            return 1;
        }
        return exitCode;
    }

    void printGrepUsage()
    {
        std::wcout << L"usage: grep [-EFHchilnRrv] [-e PATTERN] PATTERN [FILE...]\n";
    }

    bool applyGrepOption(const wchar_t option, GrepOptions& options)
    {
        switch (option)
        {
        case L'E':
            options.fixedStrings = false;
            options.extendedRegex = true;
            return true;
        case L'F':
            options.fixedStrings = true;
            options.extendedRegex = false;
            return true;
        case L'H':
            options.withFilename = true;
            options.noFilename = false;
            return true;
        case L'h':
            options.noFilename = true;
            options.withFilename = false;
            return true;
        case L'i':
            options.ignoreCase = true;
            return true;
        case L'n':
            options.lineNumber = true;
            return true;
        case L'R':
            options.followDirectorySymlinks = true;
            options.recursive = true;
            return true;
        case L'r':
            options.recursive = true;
            options.followDirectorySymlinks = false;
            return true;
        case L'v':
            options.invertMatch = true;
            return true;
        case L'c':
            options.count = true;
            options.filesWithMatches = false;
            return true;
        case L'l':
            options.filesWithMatches = true;
            options.count = false;
            return true;
        default:
            return false;
        }
    }

    bool parseGrepOptions(const std::vector<std::wstring_view>& arguments, GrepOptions& options)
    {
        bool optionsEnded = false;
        for (size_t index = 0; index < arguments.size(); ++index)
        {
            const auto argument = arguments[index];
            if (!optionsEnded && argument == L"--")
            {
                optionsEnded = true;
                continue;
            }
            if (!optionsEnded && argument == L"--help")
            {
                printGrepUsage();
                return false;
            }
            if (!optionsEnded && (argument == L"-e" || argument == L"--regexp"))
            {
                if (++index >= arguments.size())
                {
                    reportError(L"grep", {}, L"option requires a pattern");
                    return false;
                }
                options.patterns.emplace_back(arguments[index]);
                continue;
            }
            if (!optionsEnded && argument.starts_with(L"--regexp="))
            {
                options.patterns.emplace_back(argument.substr(9));
                continue;
            }
            if (!optionsEnded && argument.starts_with(L"--"))
            {
                const auto name = argument.substr(2);
                if (name == L"ignore-case")
                {
                    options.ignoreCase = true;
                }
                else if (name == L"line-number")
                {
                    options.lineNumber = true;
                }
                else if (name == L"invert-match")
                {
                    options.invertMatch = true;
                }
                else if (name == L"fixed-strings")
                {
                    options.fixedStrings = true;
                    options.extendedRegex = false;
                }
                else if (name == L"extended-regexp")
                {
                    options.fixedStrings = false;
                    options.extendedRegex = true;
                }
                else if (name == L"count")
                {
                    options.count = true;
                    options.filesWithMatches = false;
                }
                else if (name == L"files-with-matches")
                {
                    options.filesWithMatches = true;
                    options.count = false;
                }
                else if (name == L"recursive")
                {
                    options.recursive = true;
                    options.followDirectorySymlinks = false;
                }
                else
                {
                    reportError(L"grep", {}, L"unsupported option " + std::wstring{ argument });
                    return false;
                }
                continue;
            }
            if (!optionsEnded && argument.size() > 1 && argument.front() == L'-')
            {
                for (const auto option : argument.substr(1))
                {
                    if (!applyGrepOption(option, options))
                    {
                        reportError(L"grep", {}, std::wstring{ L"unsupported option -" } + option);
                        return false;
                    }
                }
                continue;
            }

            if (options.patterns.empty())
            {
                options.patterns.emplace_back(argument);
            }
            else
            {
                options.paths.emplace_back(Terminal::UnixCommandShim::ConvertLinuxPath(argument));
            }
        }

        if (options.patterns.empty())
        {
            printGrepUsage();
            return false;
        }
        if (options.recursive && options.paths.empty())
        {
            options.paths.emplace_back(L".");
        }
        return true;
    }

    bool fixedMatch(const std::string_view line,
                    const std::string_view pattern,
                    const bool ignoreCase)
    {
        if (!ignoreCase)
        {
            return line.find(pattern) != std::string_view::npos;
        }

        return std::search(line.begin(),
                           line.end(),
                           pattern.begin(),
                           pattern.end(),
                           [](const unsigned char left, const unsigned char right) {
                               const auto lower = [](const unsigned char value) {
                                   return value >= 'A' && value <= 'Z' ? static_cast<unsigned char>(value + ('a' - 'A')) : value;
                               };
                               return lower(left) == lower(right);
                           }) != line.end();
    }

    GrepResult grepStream(std::istream& stream,
                          const std::filesystem::path& path,
                          const bool showFilename,
                          const GrepOptions& options,
                          const std::vector<std::string>& fixedPatterns,
                          const std::vector<std::regex>& regexPatterns)
    {
        GrepResult result;
        size_t lineNumber = 0;
        size_t selectedCount = 0;
        std::string line;
        const auto displayPath = pathToUtf8(path);

        while (std::getline(stream, line))
        {
            ++lineNumber;
            if (!line.empty() && line.back() == '\r')
            {
                line.pop_back();
            }

            bool matched = false;
            if (options.fixedStrings)
            {
                matched = std::any_of(fixedPatterns.begin(), fixedPatterns.end(), [&](const auto& pattern) {
                    return fixedMatch(line, pattern, options.ignoreCase);
                });
            }
            else
            {
                matched = std::any_of(regexPatterns.begin(), regexPatterns.end(), [&](const auto& pattern) {
                    return std::regex_search(line, pattern);
                });
            }

            const auto selected = options.invertMatch ? !matched : matched;
            if (!selected)
            {
                continue;
            }

            result.selected = true;
            ++selectedCount;
            if (options.filesWithMatches)
            {
                std::cout << displayPath << '\n';
                break;
            }
            if (options.count)
            {
                continue;
            }
            if (showFilename)
            {
                std::cout << displayPath << ':';
            }
            if (options.lineNumber)
            {
                std::cout << lineNumber << ':';
            }
            std::cout << line << '\n';
        }

        if (stream.bad())
        {
            reportError(L"grep", path, L"failed while reading input");
            result.error = true;
        }
        if (options.count)
        {
            if (showFilename)
            {
                std::cout << displayPath << ':';
            }
            std::cout << selectedCount << '\n';
        }
        return result;
    }

    int grepCommand(const std::vector<std::wstring_view>& arguments)
    {
        GrepOptions options;
        if (!parseGrepOptions(arguments, options))
        {
            return arguments.size() == 1 && arguments.front() == L"--help" ? 0 : usageError;
        }

        std::vector<std::string> fixedPatterns;
        std::vector<std::regex> regexPatterns;
        for (const auto& pattern : options.patterns)
        {
            const auto encoded = toUtf8(pattern);
            if (!encoded)
            {
                reportError(L"grep", {}, L"pattern is not valid Unicode");
                return usageError;
            }

            if (options.fixedStrings)
            {
                fixedPatterns.emplace_back(*encoded);
            }
            else
            {
                auto flags = options.extendedRegex ?
                                 std::regex_constants::extended :
                                 std::regex_constants::basic;
                if (options.ignoreCase)
                {
                    flags |= std::regex_constants::icase;
                }
                try
                {
                    regexPatterns.emplace_back(*encoded, flags);
                }
                catch (const std::regex_error& error)
                {
                    reportError(L"grep", {}, L"invalid regular expression: " + widen(error.what(), CP_ACP));
                    return usageError;
                }
            }
        }

        bool anySelected = false;
        bool anyError = false;
        if (options.paths.empty())
        {
            const auto result = grepStream(std::cin, {}, false, options, fixedPatterns, regexPatterns);
            anySelected = result.selected;
            anyError = result.error;
        }
        else
        {
            const auto defaultShowFilename = options.withFilename ||
                                             (!options.noFilename && (options.paths.size() > 1 || options.recursive));
            for (const auto& path : options.paths)
            {
                std::error_code error;
                if (std::filesystem::is_directory(path, error))
                {
                    if (!options.recursive)
                    {
                        reportError(L"grep", path, L"is a directory; use -r to search it");
                        anyError = true;
                        continue;
                    }

                    auto directoryOptions = std::filesystem::directory_options::none;
                    if (options.followDirectorySymlinks)
                    {
                        directoryOptions |= std::filesystem::directory_options::follow_directory_symlink;
                    }
                    std::filesystem::recursive_directory_iterator iterator{ path, directoryOptions, error };
                    const std::filesystem::recursive_directory_iterator end;
                    if (error)
                    {
                        reportError(L"grep", path, error);
                        anyError = true;
                        continue;
                    }

                    std::set<std::filesystem::path, decltype(&pathLessInsensitive)> visitedDirectories{
                        &pathLessInsensitive
                    };
                    if (options.followDirectorySymlinks)
                    {
                        const auto canonicalRoot = std::filesystem::weakly_canonical(path, error);
                        if (error)
                        {
                            reportError(L"grep", path, error);
                            anyError = true;
                            continue;
                        }
                        visitedDirectories.emplace(canonicalRoot);
                    }

                    while (iterator != end)
                    {
                        const auto entry = *iterator;
                        if (options.followDirectorySymlinks && entry.is_directory(error))
                        {
                            const auto canonicalDirectory = std::filesystem::weakly_canonical(entry.path(), error);
                            if (!error && !visitedDirectories.emplace(canonicalDirectory).second)
                            {
                                iterator.disable_recursion_pending();
                            }
                        }
                        if (error)
                        {
                            reportError(L"grep", entry.path(), error);
                            error.clear();
                            anyError = true;
                        }
                        else if (entry.is_regular_file(error))
                        {
                            std::ifstream stream{ entry.path(), std::ios::binary };
                            if (!stream)
                            {
                                reportError(L"grep", entry.path(), L"could not open file");
                                anyError = true;
                            }
                            else
                            {
                                const auto result = grepStream(stream,
                                                               entry.path(),
                                                               !options.noFilename,
                                                               options,
                                                               fixedPatterns,
                                                               regexPatterns);
                                anySelected = anySelected || result.selected;
                                anyError = anyError || result.error;
                            }
                        }
                        else if (error)
                        {
                            reportError(L"grep", entry.path(), error);
                            error.clear();
                            anyError = true;
                        }

                        iterator.increment(error);
                        if (error)
                        {
                            reportError(L"grep", entry.path(), error);
                            error.clear();
                            anyError = true;
                        }
                    }
                    continue;
                }
                if (error)
                {
                    reportError(L"grep", path, error);
                    anyError = true;
                    continue;
                }

                std::ifstream stream{ path, std::ios::binary };
                if (!stream)
                {
                    reportError(L"grep", path, L"could not open file");
                    anyError = true;
                    continue;
                }
                const auto result = grepStream(stream,
                                               path,
                                               defaultShowFilename,
                                               options,
                                               fixedPatterns,
                                               regexPatterns);
                anySelected = anySelected || result.selected;
                anyError = anyError || result.error;
            }
        }

        return anyError ? 2 : anySelected ? 0 :
                                            1;
    }
}

int wmain(const int argc, wchar_t* argv[])
{
    if (argc < 1)
    {
        return usageError;
    }

    auto command = asciiLower(std::filesystem::path{ argv[0] }.stem().wstring());
    auto argumentOffset = 1;
    if (command == L"wtcmd" || command == L"unixcommandshim")
    {
        if (argc < 2)
        {
            std::wcerr << L"wtcmd: expected one of: cp, grep, mv, rm\n";
            return usageError;
        }
        command = asciiLower(argv[1]);
        argumentOffset = 2;
    }

    std::vector<std::wstring_view> arguments;
    arguments.reserve(static_cast<size_t>(argc - argumentOffset));
    for (auto index = argumentOffset; index < argc; ++index)
    {
        arguments.emplace_back(argv[index]);
    }

    if (command == L"cp")
    {
        return copyCommand(arguments);
    }
    if (command == L"cat")
    {
        return Terminal::UnixCommandShim::Cat(arguments);
    }
    if (command == L"du")
    {
        return Terminal::UnixCommandShim::Du(arguments);
    }
    if (command == L"grep")
    {
        return grepCommand(arguments);
    }
    if (command == L"head")
    {
        return Terminal::UnixCommandShim::Head(arguments);
    }
    if (command == L"mv")
    {
        return moveCommand(arguments);
    }
    if (command == L"rm")
    {
        return recycleCommand(arguments);
    }
    if (command == L"tail")
    {
        return Terminal::UnixCommandShim::Tail(arguments);
    }
    if (command == L"touch")
    {
        return Terminal::UnixCommandShim::Touch(arguments);
    }
    if (command == L"wc")
    {
        return Terminal::UnixCommandShim::Wc(arguments);
    }
    if (command == L"wt" || command == L"wtd")
    {
        return launchTerminal();
    }

    std::wcerr << L"wtcmd: unsupported command '" << command << L"'\n";
    return usageError;
}

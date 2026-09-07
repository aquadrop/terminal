// Copyright (c) Microsoft Corporation.
// Licensed under the MIT license.

#define NOMINMAX
#define WIN32_LEAN_AND_MEAN

#include <Windows.h>
#include <fcntl.h>
#include <io.h>

#include <algorithm>
#include <cerrno>
#include <cwchar>
#include <deque>
#include <filesystem>
#include <fstream>
#include <iomanip>
#include <iostream>
#include <limits>
#include <optional>
#include <set>
#include <sstream>
#include <span>
#include <string>
#include <string_view>
#include <system_error>
#include <tuple>
#include <vector>

#include "CoreFileCommands.h"
#include "PathTranslation.h"

namespace
{
    constexpr int usageError = 2;

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

    class UniqueFindHandle
    {
    public:
        explicit UniqueFindHandle(const HANDLE value = INVALID_HANDLE_VALUE) noexcept :
            _value{ value }
        {
        }

        ~UniqueFindHandle()
        {
            if (_value != INVALID_HANDLE_VALUE)
            {
                FindClose(_value);
            }
        }

        UniqueFindHandle(const UniqueFindHandle&) = delete;
        UniqueFindHandle& operator=(const UniqueFindHandle&) = delete;

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

    struct SliceOptions
    {
        bool bytes = false;
        bool quiet = false;
        bool verbose = false;
        size_t count = 10;
        std::vector<std::filesystem::path> files;
    };

    struct DuOptions
    {
        bool all = false;
        bool apparentSize = false;
        bool outputBytes = false;
        bool grandTotal = false;
        bool humanReadable = false;
        bool summarize = false;
        uint64_t blockSize = 1024;
        std::vector<std::filesystem::path> paths;
    };

    struct DuRow
    {
        uint64_t bytes;
        std::filesystem::path path;
    };

    struct UsageResult
    {
        uint64_t bytes = 0;
        bool success = false;
        bool directory = false;
    };

    struct WcCounts
    {
        uint64_t lines = 0;
        uint64_t words = 0;
        uint64_t bytes = 0;
        uint64_t characters = 0;

        WcCounts& operator+=(const WcCounts& other) noexcept
        {
            lines += other.lines;
            words += other.words;
            bytes += other.bytes;
            characters += other.characters;
            return *this;
        }
    };

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
        return toUtf8(path.native()).value_or("<invalid path>");
    }

    std::wstring widen(const std::string_view value)
    {
        if (value.empty())
        {
            return {};
        }

        const auto required = MultiByteToWideChar(CP_ACP,
                                                  0,
                                                  value.data(),
                                                  static_cast<int>(value.size()),
                                                  nullptr,
                                                  0);
        if (required <= 0)
        {
            return L"unknown error";
        }

        std::wstring result(static_cast<size_t>(required), L'\0');
        MultiByteToWideChar(CP_ACP,
                            0,
                            value.data(),
                            static_cast<int>(value.size()),
                            result.data(),
                            required);
        return result;
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
        reportError(command, path, widen(error.message()));
    }

    bool containsWildcard(const std::filesystem::path& path)
    {
        const auto& value = path.native();
        return value.find_first_of(L"*?") != std::wstring::npos;
    }

    std::vector<std::filesystem::path> expandPath(const std::filesystem::path& operand)
    {
        if (!containsWildcard(operand))
        {
            return { operand };
        }

        WIN32_FIND_DATAW data{};
        const UniqueFindHandle search{ FindFirstFileW(operand.c_str(), &data) };
        if (!search)
        {
            return { operand };
        }

        std::vector<std::filesystem::path> matches;
        const auto parent = operand.parent_path();
        do
        {
            const std::wstring_view name{ data.cFileName };
            if (name != L"." && name != L"..")
            {
                matches.emplace_back(parent.empty() ? std::filesystem::path{ name } : parent / name);
            }
        } while (FindNextFileW(search.Get(), &data));

        if (matches.empty())
        {
            return { operand };
        }
        std::sort(matches.begin(), matches.end());
        return matches;
    }

    std::vector<std::filesystem::path> expandPaths(const std::vector<std::filesystem::path>& operands)
    {
        std::vector<std::filesystem::path> expanded;
        for (const auto& operand : operands)
        {
            if (operand == L"-")
            {
                expanded.emplace_back(operand);
                continue;
            }
            auto matches = expandPath(operand);
            expanded.insert(expanded.end(),
                            std::make_move_iterator(matches.begin()),
                            std::make_move_iterator(matches.end()));
        }
        return expanded;
    }

    bool parseCount(const std::wstring_view value, size_t& result)
    {
        if (value.empty() || value.front() == L'+' || value.front() == L'-')
        {
            return false;
        }

        const std::wstring text{ value };
        wchar_t* end = nullptr;
        errno = 0;
        const auto parsed = std::wcstoull(text.c_str(), &end, 10);
        if (errno == ERANGE || !end || *end != L'\0' || parsed > std::numeric_limits<size_t>::max())
        {
            return false;
        }
        result = static_cast<size_t>(parsed);
        return true;
    }

    bool openInput(const std::wstring_view command,
                   const std::filesystem::path& path,
                   std::ifstream& file,
                   std::istream*& stream)
    {
        if (path == L"-")
        {
            stream = &std::cin;
            return true;
        }

        file.open(path, std::ios::binary);
        if (!file)
        {
            reportError(command, path, L"could not open file");
            return false;
        }
        stream = &file;
        return true;
    }

    bool copyStream(std::istream& stream)
    {
        char buffer[64 * 1024];
        while (stream.read(buffer, sizeof(buffer)) || stream.gcount() > 0)
        {
            std::cout.write(buffer, stream.gcount());
            if (!std::cout)
            {
                return false;
            }
        }
        return !stream.bad();
    }

    bool readLine(std::istream& stream, std::string& line, bool& terminated)
    {
        line.clear();
        if (!std::getline(stream, line))
        {
            terminated = false;
            return false;
        }
        terminated = !stream.eof();
        return true;
    }

    bool finishOutput(const std::wstring_view command)
    {
        std::cout.flush();
        if (!std::cout)
        {
            reportError(command, {}, L"failed while writing output");
            return false;
        }
        return true;
    }

    void setBinaryStandardStreams()
    {
        _setmode(_fileno(stdin), _O_BINARY);
        _setmode(_fileno(stdout), _O_BINARY);
    }

    bool parseSliceOptions(const std::wstring_view command,
                           const std::span<const std::wstring_view> arguments,
                           SliceOptions& options)
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
            if (!optionsEnded && (argument == L"-q" || argument == L"--quiet" || argument == L"--silent"))
            {
                options.quiet = true;
                options.verbose = false;
                continue;
            }
            if (!optionsEnded && (argument == L"-v" || argument == L"--verbose"))
            {
                options.verbose = true;
                options.quiet = false;
                continue;
            }
            if (!optionsEnded && (argument == L"-n" || argument == L"--lines" ||
                                  argument == L"-c" || argument == L"--bytes"))
            {
                options.bytes = argument == L"-c" || argument == L"--bytes";
                if (++index >= arguments.size() || !parseCount(arguments[index], options.count))
                {
                    reportError(command, {}, L"option requires a non-negative count");
                    return false;
                }
                continue;
            }
            if (!optionsEnded && argument.starts_with(L"--lines="))
            {
                options.bytes = false;
                if (!parseCount(argument.substr(8), options.count))
                {
                    reportError(command, {}, L"invalid line count");
                    return false;
                }
                continue;
            }
            if (!optionsEnded && argument.starts_with(L"--bytes="))
            {
                options.bytes = true;
                if (!parseCount(argument.substr(8), options.count))
                {
                    reportError(command, {}, L"invalid byte count");
                    return false;
                }
                continue;
            }
            if (!optionsEnded && argument.size() > 1 && argument.front() == L'-' &&
                std::all_of(argument.begin() + 1, argument.end(), [](const wchar_t ch) {
                    return ch >= L'0' && ch <= L'9';
                }))
            {
                options.bytes = false;
                if (!parseCount(argument.substr(1), options.count))
                {
                    reportError(command, {}, L"invalid line count");
                    return false;
                }
                continue;
            }
            if (!optionsEnded && argument.size() > 1 && argument.front() == L'-')
            {
                reportError(command, {}, L"unsupported option " + std::wstring{ argument });
                return false;
            }
            options.files.emplace_back(Terminal::UnixCommandShim::ConvertLinuxPath(argument));
        }

        if (options.files.empty())
        {
            options.files.emplace_back(L"-");
        }
        options.files = expandPaths(options.files);
        return true;
    }

    void printHeader(const std::filesystem::path& path, const bool first)
    {
        if (!first)
        {
            std::cout << '\n';
        }
        std::cout << "==> " << (path == L"-" ? "standard input" : pathToUtf8(path)) << " <==\n";
    }

    bool outputTailBytes(std::istream& stream,
                         std::ifstream* file,
                         const size_t count)
    {
        if (file)
        {
            file->clear();
            file->seekg(0, std::ios::end);
            const auto end = file->tellg();
            if (end >= 0)
            {
                const auto available = static_cast<uint64_t>(end);
                const auto offset = available > count ? available - count : 0;
                file->seekg(static_cast<std::streamoff>(offset), std::ios::beg);
                return copyStream(*file);
            }
            file->clear();
            file->seekg(0, std::ios::beg);
        }

        std::deque<char> data;
        char buffer[64 * 1024];
        while (stream.read(buffer, sizeof(buffer)) || stream.gcount() > 0)
        {
            const auto read = static_cast<size_t>(stream.gcount());
            for (size_t index = 0; index < read; ++index)
            {
                data.push_back(buffer[index]);
                if (data.size() > count)
                {
                    data.pop_front();
                }
            }
        }
        for (const auto ch : data)
        {
            std::cout.put(ch);
        }
        return !stream.bad() && !!std::cout;
    }

    bool outputTailLines(std::istream& stream,
                         std::ifstream* file,
                         const size_t count)
    {
        if (file)
        {
            file->clear();
            file->seekg(0, std::ios::end);
            const auto end = file->tellg();
            if (end >= 0)
            {
                auto position = static_cast<uint64_t>(end);
                std::deque<std::string> chunks;
                size_t newlineCount = 0;
                auto finalNewline = false;
                auto firstChunk = true;

                while (position > 0)
                {
                    const auto chunkSize = static_cast<size_t>(std::min<uint64_t>(position, 64 * 1024));
                    position -= chunkSize;
                    file->seekg(static_cast<std::streamoff>(position), std::ios::beg);

                    std::string chunk(chunkSize, '\0');
                    file->read(chunk.data(), static_cast<std::streamsize>(chunk.size()));
                    if (static_cast<size_t>(file->gcount()) != chunkSize)
                    {
                        return false;
                    }
                    if (firstChunk)
                    {
                        finalNewline = !chunk.empty() && chunk.back() == '\n';
                        firstChunk = false;
                    }
                    newlineCount += static_cast<size_t>(std::count(chunk.begin(), chunk.end(), '\n'));
                    chunks.emplace_front(std::move(chunk));

                    const auto requiredNewlines = finalNewline && count < std::numeric_limits<size_t>::max() ?
                                                      count + 1 :
                                                      count;
                    if (newlineCount >= requiredNewlines)
                    {
                        break;
                    }
                }

                size_t totalSize = 0;
                for (const auto& chunk : chunks)
                {
                    totalSize += chunk.size();
                }
                std::string data;
                data.reserve(totalSize);
                for (const auto& chunk : chunks)
                {
                    data.append(chunk);
                }

                const auto requiredNewlines = finalNewline && count < std::numeric_limits<size_t>::max() ?
                                                  count + 1 :
                                                  count;
                size_t seenNewlines = 0;
                size_t start = 0;
                for (size_t index = data.size(); index > 0; --index)
                {
                    if (data[index - 1] == '\n' && ++seenNewlines == requiredNewlines)
                    {
                        start = index;
                        break;
                    }
                }
                std::cout.write(data.data() + start,
                                static_cast<std::streamsize>(data.size() - start));
                return !!std::cout;
            }
            file->clear();
            file->seekg(0, std::ios::beg);
        }

        struct Line
        {
            std::string text;
            bool terminated;
        };

        std::deque<Line> lines;
        std::string line;
        bool terminated = false;
        while (readLine(stream, line, terminated))
        {
            lines.emplace_back(Line{ std::move(line), terminated });
            if (lines.size() > count)
            {
                lines.pop_front();
            }
        }
        for (const auto& output : lines)
        {
            std::cout << output.text;
            if (output.terminated)
            {
                std::cout.put('\n');
            }
        }
        return !stream.bad() && !!std::cout;
    }

    int headOrTail(const bool head,
                   const std::span<const std::wstring_view> arguments)
    {
        const std::wstring_view command = head ? L"head" : L"tail";
        SliceOptions options;
        if (!parseSliceOptions(command, arguments, options))
        {
            return usageError;
        }

        setBinaryStandardStreams();
        auto exitCode = 0;
        const auto showHeaders = options.verbose || (!options.quiet && options.files.size() > 1);
        auto first = true;
        for (const auto& path : options.files)
        {
            std::ifstream file;
            std::istream* stream = nullptr;
            if (!openInput(command, path, file, stream))
            {
                exitCode = 1;
                continue;
            }
            if (showHeaders)
            {
                printHeader(path, first);
            }
            first = false;

            if (options.count == 0)
            {
                continue;
            }

            if (options.bytes)
            {
                if (head)
                {
                    std::vector<char> buffer(std::min<size_t>(options.count, 64 * 1024));
                    auto remaining = options.count;
                    while (remaining > 0 && *stream)
                    {
                        const auto requested = std::min(remaining, buffer.size());
                        stream->read(buffer.data(), static_cast<std::streamsize>(requested));
                        const auto read = static_cast<size_t>(stream->gcount());
                        std::cout.write(buffer.data(), static_cast<std::streamsize>(read));
                        remaining -= read;
                        if (read == 0)
                        {
                            break;
                        }
                    }
                }
                else
                {
                    if (!outputTailBytes(*stream, path == L"-" ? nullptr : &file, options.count))
                    {
                        exitCode = 1;
                    }
                }
            }
            else if (head)
            {
                std::string line;
                bool terminated = false;
                for (size_t lineNumber = 0;
                     lineNumber < options.count && readLine(*stream, line, terminated);
                     ++lineNumber)
                {
                    std::cout << line;
                    if (terminated)
                    {
                        std::cout.put('\n');
                    }
                }
            }
            else
            {
                if (!outputTailLines(*stream, path == L"-" ? nullptr : &file, options.count))
                {
                    exitCode = 1;
                }
            }

            if (stream->bad())
            {
                reportError(command, path, L"failed while reading input");
                exitCode = 1;
            }
        }
        return finishOutput(command) ? exitCode : 1;
    }

    UsageResult calculateUsage(const std::filesystem::path& path,
                               const DuOptions& options,
                               const bool topLevel,
                               std::set<std::tuple<DWORD, DWORD, DWORD>>& seenFiles,
                               std::vector<DuRow>& rows,
                               bool& hadError)
    {
        const auto attributes = GetFileAttributesW(path.c_str());
        if (attributes == INVALID_FILE_ATTRIBUTES)
        {
            reportError(L"du",
                        path,
                        std::error_code{ static_cast<int>(GetLastError()), std::system_category() });
            hadError = true;
            return {};
        }

        const auto directory = (attributes & FILE_ATTRIBUTE_DIRECTORY) != 0;
        if ((attributes & FILE_ATTRIBUTE_REPARSE_POINT) != 0)
        {
            if ((directory && topLevel && !options.summarize) ||
                (options.all && !topLevel))
            {
                rows.emplace_back(DuRow{ 0, path });
            }
            return UsageResult{ 0, true, directory };
        }

        if (!directory)
        {
            const UniqueHandle file{ CreateFileW(path.c_str(),
                                                 FILE_READ_ATTRIBUTES,
                                                 FILE_SHARE_READ | FILE_SHARE_WRITE | FILE_SHARE_DELETE,
                                                 nullptr,
                                                 OPEN_EXISTING,
                                                 FILE_FLAG_OPEN_REPARSE_POINT,
                                                 nullptr) };
            if (!file)
            {
                reportError(L"du",
                            path,
                            std::error_code{ static_cast<int>(GetLastError()), std::system_category() });
                hadError = true;
                return {};
            }

            FILE_STANDARD_INFO standardInformation{};
            BY_HANDLE_FILE_INFORMATION fileInformation{};
            if (!GetFileInformationByHandleEx(file.Get(),
                                              FileStandardInfo,
                                              &standardInformation,
                                              sizeof(standardInformation)) ||
                !GetFileInformationByHandle(file.Get(), &fileInformation))
            {
                reportError(L"du",
                            path,
                            std::error_code{ static_cast<int>(GetLastError()), std::system_category() });
                hadError = true;
                return {};
            }

            const auto identity = std::make_tuple(fileInformation.dwVolumeSerialNumber,
                                                  fileInformation.nFileIndexHigh,
                                                  fileInformation.nFileIndexLow);
            const auto size = seenFiles.emplace(identity).second ?
                                  static_cast<uint64_t>(options.apparentSize ?
                                                            standardInformation.EndOfFile.QuadPart :
                                                            standardInformation.AllocationSize.QuadPart) :
                                  0;
            if (options.all && !topLevel)
            {
                rows.emplace_back(DuRow{ size, path });
            }
            return UsageResult{ size, true, false };
        }

        uint64_t total = 0;
        std::error_code error;
        std::filesystem::directory_iterator iterator{ path, error };
        const std::filesystem::directory_iterator end;
        if (error)
        {
            reportError(L"du", path, error);
            hadError = true;
            return {};
        }

        while (iterator != end)
        {
            const auto entryPath = iterator->path();
            const auto child = calculateUsage(entryPath,
                                              options,
                                              false,
                                              seenFiles,
                                              rows,
                                              hadError);
            if (child.success)
            {
                total += child.bytes;
            }
            iterator.increment(error);
            if (error)
            {
                reportError(L"du", path, error);
                hadError = true;
                break;
            }
        }

        if (!options.summarize)
        {
            rows.emplace_back(DuRow{ total, path });
        }
        return UsageResult{ total, true, true };
    }

    std::string formatSize(const uint64_t bytes, const DuOptions& options)
    {
        if (options.humanReadable)
        {
            static constexpr std::string_view units[]{ "B", "K", "M", "G", "T", "P", "E" };
            auto value = static_cast<double>(bytes);
            size_t unit = 0;
            while (value >= 1024.0 && unit + 1 < std::size(units))
            {
                value /= 1024.0;
                ++unit;
            }

            std::ostringstream output;
            if (unit == 0 || value >= 10.0)
            {
                output << static_cast<uint64_t>(value + 0.5);
            }
            else
            {
                output << std::fixed << std::setprecision(1) << value;
            }
            output << units[unit];
            return output.str();
        }
        if (options.outputBytes)
        {
            return std::to_string(bytes);
        }
        return std::to_string((bytes + options.blockSize - 1) / options.blockSize);
    }

    void printDuRow(const DuRow& row, const DuOptions& options)
    {
        std::cout << formatSize(row.bytes, options) << '\t' << pathToUtf8(row.path) << '\n';
    }

    bool isWordSeparator(const unsigned char value) noexcept
    {
        return value == ' ' || value == '\t' || value == '\r' ||
               value == '\n' || value == '\f' || value == '\v';
    }

    WcCounts countStream(std::istream& stream)
    {
        WcCounts counts;
        auto inWord = false;
        char buffer[64 * 1024];
        while (stream.read(buffer, sizeof(buffer)) || stream.gcount() > 0)
        {
            const auto read = static_cast<size_t>(stream.gcount());
            counts.bytes += read;
            for (size_t index = 0; index < read; ++index)
            {
                const auto value = static_cast<unsigned char>(buffer[index]);
                if (value == '\n')
                {
                    ++counts.lines;
                }
                if ((value & 0xc0) != 0x80)
                {
                    ++counts.characters;
                }

                const auto separator = isWordSeparator(value);
                if (inWord && separator)
                {
                    ++counts.words;
                }
                inWord = !separator;
            }
        }
        if (inWord)
        {
            ++counts.words;
        }
        return counts;
    }
}

int Terminal::UnixCommandShim::Cat(const std::span<const std::wstring_view> arguments)
{
    auto numberAll = false;
    auto numberNonblank = false;
    auto squeezeBlank = false;
    bool optionsEnded = false;
    std::vector<std::filesystem::path> files;

    for (const auto argument : arguments)
    {
        if (!optionsEnded && argument == L"--")
        {
            optionsEnded = true;
            continue;
        }
        if (!optionsEnded && argument.size() > 1 && argument.front() == L'-' && argument != L"-")
        {
            for (const auto option : argument.substr(1))
            {
                if (option == L'n')
                {
                    numberAll = true;
                }
                else if (option == L'b')
                {
                    numberNonblank = true;
                }
                else if (option == L's')
                {
                    squeezeBlank = true;
                }
                else if (option != L'u')
                {
                    reportError(L"cat", {}, std::wstring{ L"unsupported option -" } + option);
                    return usageError;
                }
            }
            continue;
        }
        files.emplace_back(Terminal::UnixCommandShim::ConvertLinuxPath(argument));
    }

    if (files.empty())
    {
        files.emplace_back(L"-");
    }
    files = expandPaths(files);
    setBinaryStandardStreams();

    size_t outputLine = 1;
    auto previousBlank = false;
    auto exitCode = 0;
    for (const auto& path : files)
    {
        std::ifstream file;
        std::istream* stream = nullptr;
        if (!openInput(L"cat", path, file, stream))
        {
            exitCode = 1;
            continue;
        }

        if (!numberAll && !numberNonblank && !squeezeBlank)
        {
            if (!copyStream(*stream))
            {
                exitCode = 1;
            }
        }
        else
        {
            std::string line;
            bool terminated = false;
            while (readLine(*stream, line, terminated))
            {
                const auto blank = line.empty() || line == "\r";
                if (squeezeBlank && blank && previousBlank)
                {
                    continue;
                }
                previousBlank = blank;

                if ((numberNonblank && !blank) || (numberAll && !numberNonblank))
                {
                    std::cout << std::setw(6) << outputLine++ << "\t";
                }
                std::cout << line;
                if (terminated)
                {
                    std::cout.put('\n');
                }
            }
        }

        if (stream->bad())
        {
            reportError(L"cat", path, L"failed while reading input");
            exitCode = 1;
        }
    }
    return finishOutput(L"cat") ? exitCode : 1;
}

int Terminal::UnixCommandShim::Du(const std::span<const std::wstring_view> arguments)
{
    DuOptions options;
    bool optionsEnded = false;
    for (const auto argument : arguments)
    {
        if (!optionsEnded && argument == L"--")
        {
            optionsEnded = true;
            continue;
        }
        if (!optionsEnded && argument.starts_with(L"--"))
        {
            if (argument == L"--all")
            {
                options.all = true;
            }
            else if (argument == L"--bytes")
            {
                options.apparentSize = true;
                options.outputBytes = true;
                options.humanReadable = false;
            }
            else if (argument == L"--total")
            {
                options.grandTotal = true;
            }
            else if (argument == L"--human-readable")
            {
                options.humanReadable = true;
                options.outputBytes = false;
            }
            else if (argument == L"--summarize")
            {
                options.summarize = true;
            }
            else
            {
                reportError(L"du", {}, L"unsupported option " + std::wstring{ argument });
                return usageError;
            }
            continue;
        }
        if (!optionsEnded && argument.size() > 1 && argument.front() == L'-')
        {
            for (const auto option : argument.substr(1))
            {
                if (option == L'a')
                {
                    options.all = true;
                }
                else if (option == L'b')
                {
                    options.apparentSize = true;
                    options.outputBytes = true;
                    options.humanReadable = false;
                }
                else if (option == L'c')
                {
                    options.grandTotal = true;
                }
                else if (option == L'h')
                {
                    options.humanReadable = true;
                    options.outputBytes = false;
                }
                else if (option == L'k')
                {
                    options.blockSize = 1024;
                    options.outputBytes = false;
                    options.humanReadable = false;
                }
                else if (option == L'm')
                {
                    options.blockSize = 1024 * 1024;
                    options.outputBytes = false;
                    options.humanReadable = false;
                }
                else if (option == L's')
                {
                    options.summarize = true;
                }
                else
                {
                    reportError(L"du", {}, std::wstring{ L"unsupported option -" } + option);
                    return usageError;
                }
            }
            continue;
        }
        options.paths.emplace_back(Terminal::UnixCommandShim::ConvertLinuxPath(argument));
    }

    if (options.all && options.summarize)
    {
        reportError(L"du", {}, L"options -a and -s cannot be combined");
        return usageError;
    }
    if (options.paths.empty())
    {
        options.paths.emplace_back(L".");
    }
    options.paths = expandPaths(options.paths);

    uint64_t grandTotal = 0;
    auto hadError = false;
    std::set<std::tuple<DWORD, DWORD, DWORD>> seenFiles;
    for (const auto& path : options.paths)
    {
        std::vector<DuRow> rows;
        const auto usage = calculateUsage(path,
                                          options,
                                          true,
                                          seenFiles,
                                          rows,
                                          hadError);
        if (!usage.success)
        {
            continue;
        }

        grandTotal += usage.bytes;
        if (options.summarize || !usage.directory)
        {
            rows.emplace_back(DuRow{ usage.bytes, path });
        }
        for (const auto& row : rows)
        {
            printDuRow(row, options);
        }
    }
    if (options.grandTotal)
    {
        printDuRow(DuRow{ grandTotal, L"total" }, options);
    }
    if (!finishOutput(L"du"))
    {
        return 1;
    }
    return hadError ? 1 : 0;
}

int Terminal::UnixCommandShim::Head(const std::span<const std::wstring_view> arguments)
{
    return headOrTail(true, arguments);
}

int Terminal::UnixCommandShim::Tail(const std::span<const std::wstring_view> arguments)
{
    return headOrTail(false, arguments);
}

int Terminal::UnixCommandShim::Touch(const std::span<const std::wstring_view> arguments)
{
    auto updateAccess = false;
    auto updateModification = false;
    auto timeSelectionSpecified = false;
    auto noCreate = false;
    bool optionsEnded = false;
    std::vector<std::filesystem::path> paths;

    for (const auto argument : arguments)
    {
        if (!optionsEnded && argument == L"--")
        {
            optionsEnded = true;
            continue;
        }
        if (!optionsEnded && argument == L"--no-create")
        {
            noCreate = true;
            continue;
        }
        if (!optionsEnded && argument.size() > 1 && argument.front() == L'-')
        {
            for (const auto option : argument.substr(1))
            {
                if (option == L'a')
                {
                    updateAccess = true;
                    timeSelectionSpecified = true;
                }
                else if (option == L'c')
                {
                    noCreate = true;
                }
                else if (option == L'm')
                {
                    updateModification = true;
                    timeSelectionSpecified = true;
                }
                else
                {
                    reportError(L"touch", {}, std::wstring{ L"unsupported option -" } + option);
                    return usageError;
                }
            }
            continue;
        }
        paths.emplace_back(Terminal::UnixCommandShim::ConvertLinuxPath(argument));
    }

    if (paths.empty())
    {
        reportError(L"touch", {}, L"missing file operand");
        return usageError;
    }
    paths = expandPaths(paths);

    FILETIME now{};
    GetSystemTimeAsFileTime(&now);
    auto exitCode = 0;
    for (const auto& path : paths)
    {
        const auto attributes = GetFileAttributesW(path.c_str());
        const auto isDirectory = attributes != INVALID_FILE_ATTRIBUTES &&
                                 (attributes & FILE_ATTRIBUTE_DIRECTORY) != 0;
        const UniqueHandle file{ CreateFileW(path.c_str(),
                                             FILE_WRITE_ATTRIBUTES,
                                             FILE_SHARE_READ | FILE_SHARE_WRITE | FILE_SHARE_DELETE,
                                             nullptr,
                                             noCreate ? OPEN_EXISTING : OPEN_ALWAYS,
                                             isDirectory ? FILE_FLAG_BACKUP_SEMANTICS : FILE_ATTRIBUTE_NORMAL,
                                             nullptr) };
        if (!file)
        {
            const auto error = GetLastError();
            if (noCreate && (error == ERROR_FILE_NOT_FOUND || error == ERROR_PATH_NOT_FOUND))
            {
                continue;
            }
            reportError(L"touch",
                        path,
                        std::error_code{ static_cast<int>(error), std::system_category() });
            exitCode = 1;
            continue;
        }

        if (!timeSelectionSpecified)
        {
            updateAccess = true;
            updateModification = true;
        }
        if (!SetFileTime(file.Get(),
                         nullptr,
                         updateAccess ? &now : nullptr,
                         updateModification ? &now : nullptr))
        {
            reportError(L"touch",
                        path,
                        std::error_code{ static_cast<int>(GetLastError()), std::system_category() });
            exitCode = 1;
        }
    }
    return exitCode;
}

int Terminal::UnixCommandShim::Wc(const std::span<const std::wstring_view> arguments)
{
    auto lines = false;
    auto words = false;
    auto bytes = false;
    auto characters = false;
    auto explicitSelection = false;
    bool optionsEnded = false;
    std::vector<std::filesystem::path> files;

    for (const auto argument : arguments)
    {
        if (!optionsEnded && argument == L"--")
        {
            optionsEnded = true;
            continue;
        }
        if (!optionsEnded && argument.size() > 1 && argument.front() == L'-' && argument != L"-")
        {
            if (!explicitSelection)
            {
                lines = words = bytes = characters = false;
                explicitSelection = true;
            }
            for (const auto option : argument.substr(1))
            {
                if (option == L'l')
                {
                    lines = true;
                }
                else if (option == L'w')
                {
                    words = true;
                }
                else if (option == L'c')
                {
                    bytes = true;
                }
                else if (option == L'm')
                {
                    characters = true;
                }
                else
                {
                    reportError(L"wc", {}, std::wstring{ L"unsupported option -" } + option);
                    return usageError;
                }
            }
            continue;
        }
        files.emplace_back(Terminal::UnixCommandShim::ConvertLinuxPath(argument));
    }

    if (!explicitSelection)
    {
        lines = words = bytes = true;
    }
    if (files.empty())
    {
        files.emplace_back(L"-");
    }
    files = expandPaths(files);
    setBinaryStandardStreams();

    const auto printCounts = [&](const WcCounts& counts, const std::filesystem::path& path) {
        if (lines)
        {
            std::cout << std::setw(8) << counts.lines;
        }
        if (words)
        {
            std::cout << std::setw(8) << counts.words;
        }
        if (characters)
        {
            std::cout << std::setw(8) << counts.characters;
        }
        if (bytes)
        {
            std::cout << std::setw(8) << counts.bytes;
        }
        if (!path.empty())
        {
            std::cout << ' ' << pathToUtf8(path);
        }
        std::cout << '\n';
    };

    WcCounts total;
    auto exitCode = 0;
    size_t successfulFiles = 0;
    for (const auto& path : files)
    {
        std::ifstream file;
        std::istream* stream = nullptr;
        if (!openInput(L"wc", path, file, stream))
        {
            exitCode = 1;
            continue;
        }
        const auto counts = countStream(*stream);
        if (stream->bad())
        {
            reportError(L"wc", path, L"failed while reading input");
            exitCode = 1;
            continue;
        }
        printCounts(counts, path == L"-" ? std::filesystem::path{} : path);
        total += counts;
        ++successfulFiles;
    }
    if (successfulFiles > 1)
    {
        printCounts(total, L"total");
    }
    return finishOutput(L"wc") ? exitCode : 1;
}

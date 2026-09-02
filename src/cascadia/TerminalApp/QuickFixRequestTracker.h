// Copyright (c) Microsoft Corporation.
// Licensed under the MIT license.

#pragma once

#include <cstdint>
#include <mutex>
#include <utility>

namespace TerminalApp
{
    class QuickFixRequestTracker final
    {
    public:
        uint64_t Start()
        {
            const std::scoped_lock lock{ _mutex };
            return ++_requestId;
        }

        template<typename Callback>
        uint64_t Start(Callback&& callback)
        {
            const std::scoped_lock lock{ _mutex };
            const auto requestId = ++_requestId;
            std::forward<Callback>(callback)();
            return requestId;
        }

        void Invalidate()
        {
            const std::scoped_lock lock{ _mutex };
            ++_requestId;
        }

        bool InvalidateIfCurrent(const uint64_t requestId)
        {
            const std::scoped_lock lock{ _mutex };
            if (_requestId != requestId)
            {
                return false;
            }

            ++_requestId;
            return true;
        }

        template<typename Callback>
        bool InvalidateIfCurrent(const uint64_t requestId, Callback&& callback)
        {
            const std::scoped_lock lock{ _mutex };
            if (_requestId != requestId)
            {
                return false;
            }

            ++_requestId;
            std::forward<Callback>(callback)();
            return true;
        }

        uint64_t Current() const
        {
            const std::scoped_lock lock{ _mutex };
            return _requestId;
        }

        bool IsCurrent(const uint64_t requestId) const
        {
            const std::scoped_lock lock{ _mutex };
            return _requestId == requestId;
        }

        template<typename Callback>
        bool RunIfCurrent(const uint64_t requestId, Callback&& callback)
        {
            const std::scoped_lock lock{ _mutex };
            if (_requestId != requestId)
            {
                return false;
            }

            std::forward<Callback>(callback)();
            return true;
        }

    private:
        mutable std::mutex _mutex;
        uint64_t _requestId{ 0 };
    };
}

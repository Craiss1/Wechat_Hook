#include "wx_send_qt.h"

#define WIN32_LEAN_AND_MEAN
#include <windows.h>

#include <MinHook.h>
#include <algorithm>
#include <cstring>
#include <mutex>

#include "global.h"

extern "C" volatile long g_wxOldReleaseState;

namespace
{
    constexpr std::uintptr_t kOrchestrator = 0x1B34D80;
    constexpr std::uintptr_t kInjectHook = 0x1B34F08;
    constexpr std::uintptr_t kRecipientHook = 0x1B38558;
    constexpr std::uintptr_t kRecipientContinue = 0x1B3855D;
    constexpr std::uintptr_t kQueueInvoke = 0x5764A0;
    constexpr std::uintptr_t kUtf8ToQString = 0x272E0;
    constexpr std::uintptr_t kCreateTextElement = 0x3A4B560;
    constexpr std::uintptr_t kAppendElement = 0x98F980;
    constexpr std::uintptr_t kWxMemcpy = 0x6A56AD0;
    constexpr std::uintptr_t kAllocator = 0x689D18C;
    constexpr std::uintptr_t kFreeObject = 0x689D1C8;
    constexpr std::uintptr_t kReleaseQString = 0x37B70;
    constexpr std::uintptr_t kReleaseContainer = 0x37E60;
    constexpr std::uintptr_t kEmptyEditorContainer = 0xA0FA868;

    constexpr unsigned char kInjectBytes[] = {0x48, 0x8D, 0x05, 0x59, 0x59, 0x5C, 0x08};
    constexpr unsigned char kRecipientBytes[] = {0xE8, 0xD3, 0x96, 0x00, 0x00};

    using GenericFn = std::int64_t(__fastcall*)(...);

    struct SlotObject
    {
        volatile long refcount;
        long reserved;
        void* implementation;
        void* controller;
    };

    struct SharedElement
    {
        void* element;
        void* control;
    };

    bool g_installed = false;
    std::mutex g_queueMutex;
    char g_recipientBuffer[0x100]{};
    char g_textBuffer[0x4000]{};
    std::uint64_t g_recipientLength = 0;
    std::uint64_t g_textLength = 0;

    void* g_fnOrchestrator = nullptr;
    void* g_fnQueueInvoke = nullptr;
    void* g_fnUtf8ToQString = nullptr;
    void* g_fnCreateTextElement = nullptr;
    void* g_fnAppendElement = nullptr;
    void* g_fnMemcpy = nullptr;
    void* g_fnAllocator = nullptr;
    void* g_fnFreeObject = nullptr;
    void* g_fnReleaseQString = nullptr;
    void* g_fnReleaseContainer = nullptr;
    void* g_emptyEditorContainer = nullptr;

    bool MatchesBytes(const void* address, const unsigned char* expected, std::size_t size)
    {
        __try
        {
            return std::memcmp(address, expected, size) == 0;
        }
        __except (EXCEPTION_EXECUTE_HANDLER)
        {
            return false;
        }
    }

    void* ReadPointer(const void* address)
    {
        __try
        {
            return *static_cast<void* const*>(address);
        }
        __except (EXCEPTION_EXECUTE_HANDLER)
        {
            return nullptr;
        }
    }

    bool IsReadableAddress(const void* address)
    {
        MEMORY_BASIC_INFORMATION memory{};
        if (!address || VirtualQuery(address, &memory, sizeof(memory)) != sizeof(memory))
            return false;
        if (memory.State != MEM_COMMIT || (memory.Protect & (PAGE_NOACCESS | PAGE_GUARD)) != 0)
            return false;
        return true;
    }

    void ReleaseControl(void* pointer)
    {
        if (!pointer)
            return;

        auto* strong = reinterpret_cast<volatile long*>(static_cast<unsigned char*>(pointer) + 0x08);
        if (InterlockedDecrement(strong) != 0)
            return;

        auto** vtable = *reinterpret_cast<void***>(pointer);
        reinterpret_cast<void(__fastcall*)(void*)>(vtable[0])(pointer);

        auto* weak = reinterpret_cast<volatile long*>(static_cast<unsigned char*>(pointer) + 0x0C);
        if (InterlockedDecrement(weak) == 0)
            reinterpret_cast<void(__fastcall*)(void*)>(vtable[1])(pointer);
    }

    void ReleaseQString(void* qstring[2])
    {
        void* data = qstring[0];
        qstring[0] = nullptr;
        qstring[1] = nullptr;
        if (!data)
            return;

        auto* refcount = static_cast<volatile long*>(data);
        const long current = *refcount;
        if (current == -1)
            return;
        if (current != 0 && InterlockedDecrement(refcount) != 0)
            return;

        reinterpret_cast<void(__fastcall*)(void*, int, int)>(g_fnReleaseQString)(data, 2, 8);
    }

    void ReleaseOldContainer(void* container)
    {
        g_wxOldReleaseState = 0;
        if (!container)
            return;

        auto* refcount = static_cast<volatile long*>(container);
        const long current = *refcount;
        if (current == -1 || (current != 0 && InterlockedDecrement(refcount) != 0))
        {
            g_wxOldReleaseState = 1;
            return;
        }

        const auto* bytes = static_cast<unsigned char*>(container);
        const auto begin = static_cast<std::int64_t>(*reinterpret_cast<const std::int32_t*>(bytes + 0x08)) * 8;
        auto pos = static_cast<std::int64_t>(*reinterpret_cast<const std::int32_t*>(bytes + 0x0C)) * 8;
        while (pos != begin)
        {
            void* record = *reinterpret_cast<void* const*>(bytes + pos + 0x08);
            if (record)
            {
                ReleaseControl(*reinterpret_cast<void**>(static_cast<unsigned char*>(record) + 0x08));
                reinterpret_cast<void(__fastcall*)(void*)>(g_fnFreeObject)(record);
            }
            pos -= 8;
        }

        reinterpret_cast<void(__fastcall*)(void*)>(g_fnReleaseContainer)(container);
        g_wxOldReleaseState = 2;
    }

    bool RewriteString(void* object, const char* source, std::size_t length)
    {
        auto* bytes = static_cast<unsigned char*>(object);
        const std::uint64_t capacity = *reinterpret_cast<std::uint64_t*>(bytes + 0x18);
        if (length > capacity)
            return false;

        void* destination = capacity < 0x10 ? object : *reinterpret_cast<void**>(object);
        reinterpret_cast<void*(__fastcall*)(void*, const void*, std::size_t)>(g_fnMemcpy)(
            destination, source, length);
        static_cast<char*>(destination)[length] = '\0';
        *reinterpret_cast<std::uint64_t*>(bytes + 0x10) = length;
        return true;
    }

    struct ControllerTrace
    {
        std::uintptr_t root = 0;
        std::uintptr_t levels[5]{};
        int candidate = -1;
        int failureLevel = 0;
    };

    struct ControllerCandidate
    {
        std::uintptr_t root;
        std::uintptr_t offsets[5]; // CE MemoryRecord order: final-first
    };

    constexpr ControllerCandidate kControllerCandidates[] = {
        {0xA8EB6D8, {0x00, 0x28, 0x198, 0x08, 0x40}},
        {0xA8EF408, {0x00, 0x28, 0x198, 0x08, 0x40}},
        {0xA9099E8, {0x00, 0x28, 0x050, 0x08, 0x40}},
        {0xA9099E8, {0x00, 0x28, 0x070, 0x08, 0x40}},
        {0xA9099E8, {0x00, 0x28, 0x078, 0x08, 0x40}},
    };

    ControllerTrace ResolveCandidate(
        std::uintptr_t moduleBase,
        int candidateIndex)
    {
        ControllerTrace trace{};
        trace.candidate = candidateIndex;
        const ControllerCandidate& candidate = kControllerCandidates[candidateIndex];
        trace.root = moduleBase + candidate.root;
        void* pointer = reinterpret_cast<void*>(trace.root);
        for (int index = 4, level = 0; index >= 0; --index, ++level)
        {
            pointer = ReadPointer(pointer);
            if (!pointer)
            {
                trace.failureLevel = level + 1;
                return trace;
            }
            pointer = static_cast<unsigned char*>(pointer) + candidate.offsets[index];
            trace.levels[level] = reinterpret_cast<std::uintptr_t>(pointer);
            if (!IsReadableAddress(pointer))
            {
                trace.failureLevel = level + 1;
                return trace;
            }
        }
        return trace;
    }

    ControllerTrace ResolveControllerTrace()
    {
        const auto base = reinterpret_cast<std::uintptr_t>(g_hWeixinDll);
        if (!base)
        {
            ControllerTrace trace{};
            trace.failureLevel = -1;
            return trace;
        }

        ControllerTrace bestFailure{};
        bestFailure.failureLevel = 1;
        for (int candidate = 0;
             candidate < static_cast<int>(std::size(kControllerCandidates));
             ++candidate)
        {
            ControllerTrace trace = ResolveCandidate(base, candidate);
            if (trace.failureLevel == 0)
                return trace;
            if (candidate == 0 || trace.failureLevel > bestFailure.failureLevel)
                bestFailure = trace;
        }
        return bestFailure;
    }
}

extern "C"
{
    void* g_wxInjectTrampoline = nullptr;
    void* g_wxRecipientTrampoline = nullptr;
    void* g_wxRecipientContinue = nullptr;

    void* g_wxController = nullptr;
    volatile long g_wxActive = 0;
    volatile long g_wxStatus = 0;
    volatile long g_wxCallbackFlag = 0;
    volatile long g_wxDestroyFlag = 0;
    volatile long g_wxInjectDone = 0;
    volatile long g_wxRecipientDone = 0;
    volatile long g_wxOldReleaseState = 0;
    void* g_wxLastTask = nullptr;

    void WxInjectDetour();
    void WxRecipientDetour();

    void __fastcall WxSlotImpl(unsigned int operation, SlotObject* slot)
    {
        if (operation == 1)
        {
            g_wxCallbackFlag = 1;
            reinterpret_cast<void(__fastcall*)(void*, int)>(g_fnOrchestrator)(slot->controller, 3);
        }
        else if (operation == 0)
        {
            g_wxDestroyFlag = 1;
            if (slot)
                reinterpret_cast<void(__fastcall*)(void*)>(g_fnFreeObject)(slot);
        }
    }

    void __fastcall WxHandleInject(void* controller, void** editorContainerSlot)
    {
        if (g_wxActive != 1 || controller != g_wxController)
            return;

        void* oldContainer = *editorContainerSlot;
        *editorContainerSlot = g_emptyEditorContainer;

        void* tempQString[2]{};
        SharedElement tempElement{};

        reinterpret_cast<GenericFn>(g_fnUtf8ToQString)(tempQString, g_textBuffer, g_textLength);
        reinterpret_cast<GenericFn>(g_fnCreateTextElement)(nullptr, &tempElement, tempQString, 1);
        reinterpret_cast<GenericFn>(g_fnAppendElement)(editorContainerSlot, &tempElement);

        void* control = tempElement.control;
        tempElement = {};
        ReleaseControl(control);
        ReleaseQString(tempQString);
        ReleaseOldContainer(oldContainer);

        g_wxInjectDone = 1;
        g_wxStatus = 2;
    }

    int __fastcall WxHandleRecipient(void* task)
    {
        if (g_wxActive != 1)
            return 0;
        if (g_wxInjectDone != 1)
            goto reject;
        if (*static_cast<void**>(task) != g_wxController)
            return 0;

        g_wxLastTask = task;
        {
            void* batch = *reinterpret_cast<void**>(static_cast<unsigned char*>(task) + 0x28);
            if (!batch)
                goto reject;

            auto* current = *static_cast<unsigned char**>(batch);
            auto* end = *reinterpret_cast<unsigned char**>(static_cast<unsigned char*>(batch) + 0x08);
            if (current >= end)
                goto reject;

            while (current < end)
            {
                void* element = *reinterpret_cast<void**>(current);
                if (!element || !RewriteString(
                        static_cast<unsigned char*>(element) + 0xB0,
                        g_recipientBuffer,
                        static_cast<std::size_t>(g_recipientLength)))
                    goto reject;
                current += 0x10;
            }
        }

        if (!RewriteString(
                static_cast<unsigned char*>(task) + 0x08,
                g_recipientBuffer,
                static_cast<std::size_t>(g_recipientLength)))
            goto reject;

        g_wxRecipientDone = 1;
        g_wxStatus = 3;
        InterlockedExchange(&g_wxActive, 0);
        return 0;

    reject:
        g_wxStatus = -4;
        InterlockedExchange(&g_wxActive, 0);
        return 1;
    }
}

namespace WeixinSend
{
    std::uintptr_t ResolveQtTextController()
    {
        // Try the shortest static Weixin.dll candidates that survived all
        // three pointer-map rescans. Their offsets are final-first.
        const ControllerTrace trace = ResolveControllerTrace();
        return trace.failureLevel == 0 ? trace.levels[4] : 0;
    }

    bool InitializeQtTextSender()
    {
        if (g_installed)
            return true;

        const auto base = reinterpret_cast<std::uintptr_t>(g_hWeixinDll);
        if (!base)
            return false;
        if (!MatchesBytes(reinterpret_cast<void*>(base + kInjectHook), kInjectBytes, sizeof(kInjectBytes)) ||
            !MatchesBytes(reinterpret_cast<void*>(base + kRecipientHook), kRecipientBytes, sizeof(kRecipientBytes)))
        {
            g_wxStatus = -5;
            return false;
        }

        g_fnOrchestrator = reinterpret_cast<void*>(base + kOrchestrator);
        g_fnQueueInvoke = reinterpret_cast<void*>(base + kQueueInvoke);
        g_fnUtf8ToQString = reinterpret_cast<void*>(base + kUtf8ToQString);
        g_fnCreateTextElement = reinterpret_cast<void*>(base + kCreateTextElement);
        g_fnAppendElement = reinterpret_cast<void*>(base + kAppendElement);
        g_fnMemcpy = reinterpret_cast<void*>(base + kWxMemcpy);
        g_fnAllocator = reinterpret_cast<void*>(base + kAllocator);
        g_fnFreeObject = reinterpret_cast<void*>(base + kFreeObject);
        g_fnReleaseQString = reinterpret_cast<void*>(base + kReleaseQString);
        g_fnReleaseContainer = reinterpret_cast<void*>(base + kReleaseContainer);
        g_emptyEditorContainer = reinterpret_cast<void*>(base + kEmptyEditorContainer);
        g_wxRecipientContinue = reinterpret_cast<void*>(base + kRecipientContinue);

        void* injectTarget = reinterpret_cast<void*>(base + kInjectHook);
        void* recipientTarget = reinterpret_cast<void*>(base + kRecipientHook);
        if (MH_CreateHook(injectTarget, WxInjectDetour, &g_wxInjectTrampoline) != MH_OK)
            return false;
        if (MH_CreateHook(recipientTarget, WxRecipientDetour, &g_wxRecipientTrampoline) != MH_OK)
        {
            MH_RemoveHook(injectTarget);
            return false;
        }
        if (MH_EnableHook(injectTarget) != MH_OK || MH_EnableHook(recipientTarget) != MH_OK)
        {
            MH_DisableHook(injectTarget);
            MH_DisableHook(recipientTarget);
            MH_RemoveHook(injectTarget);
            MH_RemoveHook(recipientTarget);
            return false;
        }

        g_installed = true;
        return true;
    }

    bool QueueQtText(const std::string& recipient, const std::string& text)
    {
        std::lock_guard<std::mutex> lock(g_queueMutex);

        if (!g_installed || recipient.empty() || text.empty() ||
            recipient.size() > sizeof(g_recipientBuffer) - 2 ||
            text.size() > sizeof(g_textBuffer) - 2)
            return false;
        if (g_wxActive != 0)
        {
            g_wxStatus = -6;
            return false;
        }

        const std::uintptr_t controller = ResolveQtTextController();
        if (!controller)
        {
            g_wxStatus = -1;
            return false;
        }

        g_wxController = reinterpret_cast<void*>(controller);
        std::memcpy(g_recipientBuffer, recipient.data(), recipient.size());
        g_recipientBuffer[recipient.size()] = '\0';
        std::memcpy(g_textBuffer, text.data(), text.size());
        g_textBuffer[text.size()] = '\0';
        g_recipientLength = recipient.size();
        g_textLength = text.size();

        g_wxStatus = 1;
        g_wxCallbackFlag = 0;
        g_wxDestroyFlag = 0;
        g_wxInjectDone = 0;
        g_wxRecipientDone = 0;
        g_wxOldReleaseState = 0;
        g_wxLastTask = nullptr;

        auto* slot = reinterpret_cast<SlotObject*>(
            reinterpret_cast<void*(__fastcall*)(std::size_t)>(g_fnAllocator)(sizeof(SlotObject)));
        if (!slot)
        {
            g_wxStatus = -2;
            return false;
        }

        slot->refcount = 1;
        slot->reserved = 0;
        slot->implementation = reinterpret_cast<void*>(WxSlotImpl);
        slot->controller = g_wxController;

        InterlockedExchange(&g_wxActive, 1);
        reinterpret_cast<GenericFn>(g_fnQueueInvoke)(nullptr, nullptr, g_wxController, slot);
        return true;
    }

    QtTextSendState GetQtTextSendState()
    {
        const ControllerTrace trace = ResolveControllerTrace();
        return {
            g_installed,
            trace.root,
            {
                trace.levels[0],
                trace.levels[1],
                trace.levels[2],
                trace.levels[3],
                trace.levels[4]
            },
            trace.candidate,
            trace.failureLevel,
            trace.failureLevel == 0 ? trace.levels[4] : 0,
            g_wxActive,
            g_wxStatus,
            g_wxCallbackFlag,
            g_wxInjectDone,
            g_wxRecipientDone,
            g_wxDestroyFlag,
            g_wxOldReleaseState,
            reinterpret_cast<std::uintptr_t>(g_wxLastTask)
        };
    }
}

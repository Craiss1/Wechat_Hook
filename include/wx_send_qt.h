#pragma once

#include <cstdint>
#include <string>

namespace WeixinSend
{
    struct QtTextSendState
    {
        bool installed;
        std::uintptr_t controllerRoot;
        std::uintptr_t controllerLevels[5];
        int controllerCandidate;
        int controllerFailureLevel;
        std::uintptr_t controller;
        long active;
        long status;
        long callback;
        long inject;
        long recipient;
        long destroy;
        long oldRelease;
        std::uintptr_t lastTask;
    };

    bool InitializeQtTextSender();
    std::uintptr_t ResolveQtTextController();
    bool QueueQtText(const std::string& recipient, const std::string& text);
    QtTextSendState GetQtTextSendState();
}

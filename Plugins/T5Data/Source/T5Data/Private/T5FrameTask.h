#pragma once

#include "CoreMinimal.h"
#include <coroutine>

// Game-thread work that owns its locals across frame boundaries. Destroying the
// task cancels it and runs the frame's normal RAII cleanup on the game thread.
class FT5FrameTask
{
public:
    struct promise_type
    {
        bool Success = false;
        FT5FrameTask get_return_object() { return FT5FrameTask(std::coroutine_handle<promise_type>::from_promise(*this)); }
        std::suspend_always initial_suspend() noexcept { return {}; }
        std::suspend_always final_suspend() noexcept { return {}; }
        std::suspend_always yield_value(int) noexcept { return {}; }
        void return_value(bool Value) noexcept { Success = Value; }
        void unhandled_exception() { UE_LOG(LogTemp, Fatal, TEXT("Unhandled T5 scene build exception")); }
    };
    explicit FT5FrameTask(std::coroutine_handle<promise_type> In) : Frame(In) {}
    FT5FrameTask(FT5FrameTask&& Other) noexcept : Frame(Other.Frame) { Other.Frame = {}; }
    FT5FrameTask(const FT5FrameTask&) = delete;
    ~FT5FrameTask() { if (Frame) Frame.destroy(); }
    void Resume() { check(IsInGameThread()); if (Frame && !Frame.done()) Frame.resume(); }
    bool Done() const { return !Frame || Frame.done(); }
    bool Succeeded() const { return Frame && Frame.done() && Frame.promise().Success; }
private:
    std::coroutine_handle<promise_type> Frame;
};

#pragma once

#include "SC_PlugIn.hpp"
#include "onda.h"

#include <cstdint>
#include <string>
#include <vector>

enum class OndaInputKind : uint8_t {
    Input = 0,
    Param = 1,
    Event = 2,
    Buffer = 3,
};

struct OndaInputDescriptor {
    std::string name;
    OndaInputKind kind = OndaInputKind::Input;
    bool audioRate = false;
    int ondaIndex = -1;
    int elemType = -1;
    int elemBytes = 0;
    int arrayLen = 1;
    bool hasInit = false;
    float init = 0.0f;
    int bufferChannelsKind = -1;
    int bufferChannelsStatic = -1;
    bool bufferMayWrite = false;
};

struct CompiledProgram {
    struct PreallocatedInstance {
        onda_instance_t* instance = nullptr;
        class Onda* unit = nullptr;
    };

    onda_program_t* program = nullptr;
    std::vector<OndaInputDescriptor> inputs;
    std::vector<OndaInputDescriptor> outputs;
    std::vector<PreallocatedInstance> instances;
    int requiredInputChannels = 0;
    int requiredOutputChannels = 0;
    int outputChannels = 0;
};

struct PatchEntry {
    int hash = 0;
    CompiledProgram* program = nullptr;
    bool active = false;
};

class Onda : public SCUnit {
public:
    Onda();
    ~Onda();

    static std::vector<PatchEntry> patchStorage;

    static CompiledProgram* getProgramByHash(int hash);
    static CompiledProgram* insertOrUpdateProgram(int hash, CompiledProgram* program);
    static CompiledProgram* removeProgram(int hash);

    void handleHotSwap(int hash, CompiledProgram* program);
    void handleFree(int hash);
    void setSilence();

private:
    enum class ProcessAudioResult : uint8_t {
        Ok = 0,
        SoftSilence = 1,
        Fatal = 2,
    };

    enum class BufferPrepResult : uint8_t {
        Ok = 0,
        Invalid = 1,
        Fatal = 2,
    };

    struct RuntimeOutputState {
        bool mapped = false;
        int scOffset = -1;
        bool directBind = false;
        uint8_t* scratch = nullptr;
        int scratchBytes = 0;
        void* boundPtr = nullptr;
        int boundBytes = 0;
        bool bound = false;
    };

    struct RuntimeInputState {
        const void* boundPtr = nullptr;
        int boundBytes = 0;
        bool bound = false;
    };

    struct RuntimeBufferState {
        void* boundPtr = nullptr;
        int boundBufIndex = -1;
        bool bound = false;
    };

#if SUPERNOVA
    struct BufferLockState {
        SndBuf* buf = nullptr;
        bool exclusive = false;
    };
#endif

    bool bindProgram(CompiledProgram* program, bool isHotSwap);
    bool bindLatestProgram(bool isHotSwap);
    bool claimInstance(CompiledProgram* program, onda_instance_t*& outInstance, int& outIndex);
    void releaseInstance();
    bool allocateRtState(CompiledProgram* program);
    void freeRtState();
    ProcessAudioResult processAudio(int nSamples);
    BufferPrepResult prepareBuffers();
    bool prepareInputs(int nSamples);
    bool prepareParamsAndEvents();
    bool prepareOutputs(int nSamples);
    void copyOutputsToSC(int nSamples);
    void silenceBlockOutputs(int nSamples);
    SndBuf* resolveSndBufByIndex(int bufIndex) const;
    bool isValidBufferBinding(const SndBuf* buf, const OndaInputDescriptor& desc) const;
    
#if SUPERNOVA
    void acquireBufferLocks();
    void releaseBufferLocks();
    bool addOrUpgradeBufferLock(SndBuf* buf, bool exclusive);
#endif

    void next(int nSamples);
    void nextSilence(int nSamples);

    int mHash = 0;
    int mInstanceSlot = -1;
    onda_instance_t* mInstance = nullptr;
    CompiledProgram* mProgram = nullptr;
    bool mBindingsNeedValidate = false;
    const OndaInputDescriptor* mBoundInputs = nullptr;
    int mBoundInputCount = 0;
    const OndaInputDescriptor* mBoundOutputs = nullptr;
    int mBoundOutputCount = 0;
    int* mAudioInputDescIndices = nullptr;
    int mAudioInputDescCount = 0;
    int* mParamDescIndices = nullptr;
    int mParamDescCount = 0;
    int* mEventDescIndices = nullptr;
    int mEventDescCount = 0;
    int* mBufferDescIndices = nullptr;
    int mBufferDescCount = 0;
    int* mScInputSlotByDesc = nullptr;
    RuntimeInputState* mRuntimeInputs = nullptr;
    RuntimeBufferState* mRuntimeBuffers = nullptr;
    RuntimeOutputState* mRuntimeOutputs = nullptr;
    uint8_t* mOutputScratchBlock = nullptr;
    bool mNeedsOutputCopy = false;
#if SUPERNOVA
    BufferLockState* mBufferLocks = nullptr;
    int mBufferLockCapacity = 0;
    int mBufferLockCount = 0;
    bool mBufferLocksHeld = false;
#endif
};

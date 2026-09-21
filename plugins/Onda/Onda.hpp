#pragma once

#include "SC_PlugIn.hpp"
#include "onda.h"

#include <cstdint>
#include <optional>
#include <string>
#include <vector>

enum class OndaInputKind : uint8_t {
    Input = 0,
    Param = 1,
    Event = 2,
    Buffer = 3,
};

struct OndaParamSpec {
    double minimum = 0.0;
    double maximum = 1.0;
    int scale = ONDA_PARAM_SCALE_LINEAR;
    std::optional<double> curve;
    std::optional<double> step;
    std::string unit;
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
    bool hasProjectDefault = false;
    std::optional<OndaParamSpec> paramSpec;
};

class Onda;

struct CompiledProgram {
    onda_program_t* program = nullptr;
    std::vector<OndaInputDescriptor> inputs;
    std::vector<OndaInputDescriptor> outputs;
    Onda* firstUnit = nullptr;
    int requiredInputChannels = 0;
    int requiredOutputChannels = 0;
    bool hasDelegates = false;
    bool hasPrints = false;
};

struct PatchEntry {
    int generation = 0;
    CompiledProgram* program = nullptr;
};

enum class ProgramPublishStatus : uint8_t {
    Inserted,
    Replaced,
    Stale,
    InvalidDefinition,
};

struct ProgramPublishResult {
    ProgramPublishStatus status = ProgramPublishStatus::InvalidDefinition;
    CompiledProgram* replacedProgram = nullptr;
};

class Onda : public SCUnit {
public:
    Onda();
    ~Onda();

    static std::vector<PatchEntry> patchStorage;

    static CompiledProgram* getProgramById(int definitionId);
    static void observeGeneration(int definitionId, int generation);
    static ProgramPublishResult publishProgram(int definitionId, int generation, CompiledProgram* program);
    static CompiledProgram* removeProgram(int definitionId, int generation);

    void handleHotSwap(int definitionId, CompiledProgram* program);
    void handleFree(int definitionId);
    void setSilence();
    Onda* nextProgramUnit() const { return mNextProgramUnit; }

private:
    enum class BufferPrepResult : uint8_t {
        Ok = 0,
        Invalid = 1,
        Fatal = 2,
    };

    struct RuntimeAudioInputState {
        int descriptorIndex = -1;
        const void* boundPtr = nullptr;
        int boundBytes = 0;
        bool bound = false;
    };

    struct RuntimeControlState {
        int descriptorIndex = -1;
        float previousValue = 0.0f;
        bool initialized = false;
    };

    struct RuntimeBufferState {
        int descriptorIndex = -1;
        void* boundPtr = nullptr;
        int boundBufIndex = -1;
        int boundFrames = -1;
        int boundChannels = -1;
        float boundSampleRate = 0.0f;
        bool bound = false;
        bool invalidReported = false;
    };

    struct RuntimeOutputState {
        int scOffset = -1;
        bool directBind = false;
        uint8_t* scratch = nullptr;
        int scratchBytes = 0;
        void* boundPtr = nullptr;
        int boundBytes = 0;
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
    bool initializeInstance();
    void attachToProgram(CompiledProgram* program);
    void detachFromProgram();
    void releaseInstance();
    bool allocateRtState(CompiledProgram* program);
    void freeRtState();
    bool processAudio(int nSamples);
    BufferPrepResult prepareBuffers();
    bool prepareInputs();
    bool prepareParams();
    bool triggerEvents();
    bool prepareOutputs();
    void flushExecutionOutput();
    void emitDelegateOccurrence(const onda_delegate_occurrence_t& occurrence) const;
    void emitPrintOccurrence(const onda_print_occurrence_t& occurrence) const;
    onda_execution_output_t* executionOutput();
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

    int mDefinitionId = 0;
    int mUnitIndex = -1;
    onda_instance_t* mInstance = nullptr;
    CompiledProgram* mProgram = nullptr;
    Onda* mPreviousProgramUnit = nullptr;
    Onda* mNextProgramUnit = nullptr;
    void* mRtStorage = nullptr;
    RuntimeAudioInputState* mRuntimeAudioInputs = nullptr;
    int mAudioInputDescCount = 0;
    RuntimeControlState* mRuntimeParams = nullptr;
    int mParamDescCount = 0;
    RuntimeControlState* mRuntimeEvents = nullptr;
    int mEventDescCount = 0;
    RuntimeBufferState* mRuntimeBuffers = nullptr;
    int mBufferDescCount = 0;
    RuntimeOutputState* mRuntimeOutputs = nullptr;
    uint8_t* mDelegateBatchStorage = nullptr;
    uint8_t* mPrintBatchStorage = nullptr;
    char* mDelegateFormatStorage = nullptr;
    char* mPrintFormatStorage = nullptr;
    onda_delegate_batch_t mDelegateBatch{};
    onda_print_batch_t mPrintBatch{};
    onda_execution_output_t mExecutionOutput{};
    bool mBindingsNeedValidate = false;
    bool mNeedsOutputCopy = false;
#if SUPERNOVA
    BufferLockState* mBufferLocks = nullptr;
    int mBufferLockCapacity = 0;
    int mBufferLockCount = 0;
    bool mBufferLocksHeld = false;
#endif
};

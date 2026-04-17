
#include "Onda.hpp"

#include <algorithm>
#include <cstdio>
#include <cstring>
#include <filesystem>
#include <fstream>
#include <optional>
#include <sstream>
#include <string>
#include <utility>
#include <vector>

static InterfaceTable* ft;

std::vector<PatchEntry> Onda::patchStorage;

namespace {

constexpr int kMaxReplySize = 4096;
constexpr int kPatchStorageCapacity = 2048;

const char* inputKindToTag(OndaInputKind kind) {
    switch (kind) {
        case OndaInputKind::Input:
            return "0";
        case OndaInputKind::Param:
            return "1";
        case OndaInputKind::Event:
            return "2";
        case OndaInputKind::Buffer:
            return "3";
    }

    return "0";
}

std::optional<std::string> readFileContent(const std::string& path) {
    std::ifstream file(path, std::ios::in | std::ios::binary);
    if (!file.is_open()) {
        return std::nullopt;
    }

    std::ostringstream ss;
    ss << file.rdbuf();
    return ss.str();
}

void destroyCompiledProgram(CompiledProgram* program) {
    if (!program) {
        return;
    }

    for (auto& entry : program->instances) {
        if (entry.instance) {
            onda_instance_destroy(entry.instance);
            entry.instance = nullptr;
            entry.unit = nullptr;
        }
    }

    if (program->program) {
        onda_program_destroy(program->program);
        program->program = nullptr;
    }

    delete program;
}

bool buildProgramMetadata(CompiledProgram& compiled, std::string& error) {
    if (!compiled.program) {
        error = "Compiled program handle is null.";
        return false;
    }

    const int bufferCount = onda_buffer_count(compiled.program);
    const int inputCount = onda_input_count(compiled.program);
    const int paramCount = onda_param_count(compiled.program);
    const int eventCount = onda_event_count(compiled.program);
    const int outputCount = onda_output_count(compiled.program);

    if (inputCount < 0 || paramCount < 0 || eventCount < 0 || bufferCount < 0 || outputCount < 0) {
        error = "Failed to query endpoint counts from Onda program.";
        return false;
    }

    compiled.inputs.clear();
    compiled.outputs.clear();
    compiled.requiredInputChannels = 0;
    compiled.requiredOutputChannels = 0;
    compiled.outputChannels = 0;

    compiled.inputs.reserve(static_cast<size_t>(inputCount + paramCount + eventCount + bufferCount));
    compiled.outputs.reserve(static_cast<size_t>(outputCount));

    for (int i = 0; i < inputCount; ++i) {
        const int arrayLen = onda_input_array_len(compiled.program, i);
        if (arrayLen > 1) {
            std::ostringstream msg;
            msg << "Host constraint: SuperCollider integration does not support input arrays ('";
            if (const char* name = onda_input_name(compiled.program, i)) {
                msg << name;
            } else {
                msg << i;
            }
            msg << "').";
            error = msg.str();
            return false;
        }

        const int elemType = onda_input_elem_type(compiled.program, i);
        if (elemType != ONDA_PRIMITIVE_F32) {
            std::ostringstream msg;
            msg << "Host constraint: input '";
            if (const char* name = onda_input_name(compiled.program, i)) {
                msg << name;
            } else {
                msg << i;
            }
            msg << "' must use f32 for SuperCollider integration.";
            error = msg.str();
            return false;
        }

        OndaInputDescriptor desc;
        desc.name = onda_input_name(compiled.program, i) ? onda_input_name(compiled.program, i) : ("in" + std::to_string(i + 1));
        desc.audioRate = true;
        desc.kind = OndaInputKind::Input;
        desc.ondaIndex = i;
        desc.elemType = ONDA_PRIMITIVE_F32;
        desc.elemBytes = static_cast<int>(sizeof(float));
        desc.arrayLen = 1;

        if (onda_input_has_default(compiled.program, i) > 0) {
            desc.hasInit = true;
            desc.init = static_cast<float>(onda_input_default_f64(compiled.program, i));
        }

        compiled.requiredInputChannels += 1;
        compiled.inputs.push_back(std::move(desc));
    }

    for (int i = 0; i < paramCount; ++i) {
        const int arrayLen = onda_param_array_len(compiled.program, i);
        if (arrayLen > 1) {
            std::ostringstream msg;
            msg << "Host constraint: SuperCollider integration does not support param arrays ('";
            if (const char* name = onda_param_name(compiled.program, i)) {
                msg << name;
            } else {
                msg << i;
            }
            msg << "').";
            error = msg.str();
            return false;
        }

        const int elemType = onda_param_elem_type(compiled.program, i);
        if (elemType != ONDA_PRIMITIVE_F32) {
            std::ostringstream msg;
            msg << "Host constraint: param '";
            if (const char* name = onda_param_name(compiled.program, i)) {
                msg << name;
            } else {
                msg << i;
            }
            msg << "' must use f32 for SuperCollider integration.";
            error = msg.str();
            return false;
        }

        OndaInputDescriptor desc;
        desc.name = onda_param_name(compiled.program, i) ? onda_param_name(compiled.program, i) : ("param" + std::to_string(i + 1));
        desc.audioRate = false;
        desc.kind = OndaInputKind::Param;
        desc.ondaIndex = i;
        desc.elemType = ONDA_PRIMITIVE_F32;
        desc.elemBytes = static_cast<int>(sizeof(float));
        desc.arrayLen = 1;

        if (onda_param_has_default(compiled.program, i) > 0) {
            desc.hasInit = true;
            desc.init = static_cast<float>(onda_param_default_f64(compiled.program, i));
        }

        compiled.inputs.push_back(std::move(desc));
    }

    for (int i = 0; i < eventCount; ++i) {
        const int eventParamCount = onda_event_param_count(compiled.program, i);
        const int payloadBytes = onda_event_payload_bytes(compiled.program, i);
        const int elemType = onda_event_param_elem_type(compiled.program, i, 0);
        const int arrayLen = onda_event_param_array_len(compiled.program, i, 0);
        const int isSlice = onda_event_param_is_slice(compiled.program, i, 0);

        if (eventParamCount != 1
            || payloadBytes != static_cast<int>(sizeof(float))
            || elemType != ONDA_PRIMITIVE_F32
            || arrayLen != 1
            || isSlice != 0) {
            std::ostringstream msg;
            msg << "Host constraint: event '";
            if (const char* name = onda_event_name(compiled.program, i)) {
                msg << name;
            } else {
                msg << i;
            }
            msg << "' must use a single scalar f32 payload for SuperCollider integration.";
            error = msg.str();
            return false;
        }

        OndaInputDescriptor desc;
        desc.name = onda_event_name(compiled.program, i) ? onda_event_name(compiled.program, i) : ("event" + std::to_string(i + 1));
        desc.audioRate = false;
        desc.kind = OndaInputKind::Event;
        desc.ondaIndex = i;
        desc.elemType = ONDA_PRIMITIVE_F32;
        desc.elemBytes = static_cast<int>(sizeof(float));
        desc.arrayLen = 1;

        if (onda_event_param_has_default(compiled.program, i, 0) > 0) {
            float defaultValue = 0.0f;
            const int copied = onda_event_param_default_bytes(
                compiled.program,
                i,
                0,
                &defaultValue,
                static_cast<int>(sizeof(defaultValue)));
            if (copied != static_cast<int>(sizeof(defaultValue))) {
                std::ostringstream msg;
                msg << "Failed to query default value for event '";
                msg << desc.name;
                msg << "'.";
                error = msg.str();
                return false;
            }
            desc.hasInit = true;
            desc.init = defaultValue;
        }

        compiled.inputs.push_back(std::move(desc));
    }

    for (int i = 0; i < bufferCount; ++i) {
        const int elemType = onda_buffer_elem_type(compiled.program, i);
        if (elemType != ONDA_PRIMITIVE_F32) {
            std::ostringstream msg;
            msg << "Host constraint: buffer '";
            if (const char* name = onda_buffer_name(compiled.program, i)) {
                msg << name;
            } else {
                msg << i;
            }
            msg << "' must use f32 elements for SuperCollider integration.";
            error = msg.str();
            return false;
        }

        const int channelsKind = onda_buffer_channels_kind(compiled.program, i);
        const int channelsStatic = onda_buffer_channels_static(compiled.program, i);
        const int mayWrite = onda_buffer_may_write(compiled.program, i);
        if (channelsKind != ONDA_BUFFER_CHANNELS_MONO
            && channelsKind != ONDA_BUFFER_CHANNELS_STATIC
            && channelsKind != ONDA_BUFFER_CHANNELS_DYNAMIC) {
            std::ostringstream msg;
            msg << "Unsupported buffer channel declaration kind at index " << i << ".";
            error = msg.str();
            return false;
        }

        if (mayWrite < 0) {
            std::ostringstream msg;
            msg << "Failed to query may_write metadata for buffer at index " << i << ".";
            error = msg.str();
            return false;
        }

        if (channelsKind == ONDA_BUFFER_CHANNELS_STATIC && channelsStatic < 1) {
            std::ostringstream msg;
            msg << "Buffer with static channels must declare at least 1 channel at index " << i << ".";
            error = msg.str();
            return false;
        }

        OndaInputDescriptor desc;
        desc.name = onda_buffer_name(compiled.program, i) ? onda_buffer_name(compiled.program, i) : ("buffer" + std::to_string(i + 1));
        desc.audioRate = false;
        desc.kind = OndaInputKind::Buffer;
        desc.ondaIndex = i;
        desc.elemType = ONDA_PRIMITIVE_F32;
        desc.elemBytes = static_cast<int>(sizeof(float));
        desc.arrayLen = 1;
        desc.hasInit = true;
        desc.init = 0.0f;
        desc.bufferChannelsKind = channelsKind;
        desc.bufferChannelsStatic = channelsStatic;
        desc.bufferMayWrite = (mayWrite > 0);

        compiled.inputs.push_back(std::move(desc));
    }

    for (int i = 0; i < outputCount; ++i) {
        int arrayLen = onda_output_array_len(compiled.program, i);
        if (arrayLen < 1) {
            arrayLen = 1;
        }

        const int elemType = onda_output_elem_type(compiled.program, i);
        if (elemType != ONDA_PRIMITIVE_F32) {
            std::ostringstream msg;
            msg << "Host constraint: output '";
            if (const char* name = onda_output_name(compiled.program, i)) {
                msg << name;
            } else {
                msg << i;
            }
            msg << "' must use f32 or f32[N] for SuperCollider integration.";
            error = msg.str();
            return false;
        }

        const int typeBytes = onda_output_type_bytes(compiled.program, i);
        if (typeBytes > 0) {
            const int expected = static_cast<int>(sizeof(float)) * arrayLen;
            if (typeBytes != expected) {
                std::ostringstream msg;
                msg << "Unexpected output byte layout at index " << i << ".";
                error = msg.str();
                return false;
            }
        }

        OndaInputDescriptor outDesc;
        outDesc.name = onda_output_name(compiled.program, i) ? onda_output_name(compiled.program, i) : ("out" + std::to_string(i + 1));
        outDesc.ondaIndex = i;
        outDesc.elemType = ONDA_PRIMITIVE_F32;
        outDesc.elemBytes = static_cast<int>(sizeof(float));
        outDesc.arrayLen = arrayLen;

        compiled.requiredOutputChannels += arrayLen;
        compiled.outputChannels += arrayLen;
        compiled.outputs.push_back(std::move(outDesc));
    }

    return true;
}

void writeReply(std::string& reply, int hash, const CompiledProgram& compiled) {
    reply = "_onda/";
    reply += std::to_string(hash);
    reply += "/";
    reply += std::to_string(compiled.inputs.size());

    for (const auto& input : compiled.inputs) {
        reply += "/";
        reply += input.name;
        reply += "/";
        reply += input.audioRate ? "0" : "1";

        reply += "/_kind/";
        reply += inputKindToTag(input.kind);

        if (input.hasInit) {
            reply += "/_init/";
            reply += std::to_string(input.init);
        }

    }

    reply += "/";
    reply += std::to_string(compiled.outputChannels);
}

struct OndaCompileCmdData {
    int hash = 0;
    int numAllocate = 0;
    char* path = nullptr;
    char replyMsg[kMaxReplySize + 1] = {0};
    CompiledProgram* newProgram = nullptr;
    CompiledProgram* oldProgram = nullptr;
};

// RT cleanup stage (audio thread): release command payload allocated with RTAlloc.
void ondaCompileCleanup(World* world, void* inUserData) {
    auto* cmdData = static_cast<OndaCompileCmdData*>(inUserData);
    if (!cmdData) {
        return;
    }

    if (cmdData->path) {
        RTFree(world, cmdData->path);
    }

    RTFree(world, cmdData);
}

// NRT stage 4 (worker thread): destroy replaced/failed compiled programs.
bool ondaCompileStage4(World* /*world*/, void* inUserData) {
    auto* cmdData = static_cast<OndaCompileCmdData*>(inUserData);
    if (!cmdData) {
        return true;
    }

    destroyCompiledProgram(cmdData->newProgram);
    cmdData->newProgram = nullptr;

    destroyCompiledProgram(cmdData->oldProgram);
    cmdData->oldProgram = nullptr;

    return true;
}

// RT stage 3 (audio thread): publish program swap and notify live units.
bool ondaCompileStage3(World* /*world*/, void* inUserData) {
    auto* cmdData = static_cast<OndaCompileCmdData*>(inUserData);
    if (!cmdData || !cmdData->newProgram) {
        return true;
    }

    CompiledProgram* insertedProgram = cmdData->newProgram;
    CompiledProgram* result = Onda::insertOrUpdateProgram(cmdData->hash, insertedProgram);

    if (result == insertedProgram) {
        // Storage full. Keep insertedProgram in cmdData->newProgram so stage4 deletes it.
        cmdData->oldProgram = nullptr;
        return true;
    }

    cmdData->oldProgram = result;

    if (result) {
        for (const auto& slot : result->instances) {
            Onda* unit = slot.unit;
            if (unit) {
                unit->handleHotSwap(cmdData->hash, insertedProgram);
            }
        }
    }

    // Program is now owned by patch storage.
    cmdData->newProgram = nullptr;
    return true;
}

// NRT stage 2 (worker thread): compile source and preallocate Onda instances.
bool ondaCompileStage2(World* world, void* inUserData) {
    auto* cmdData = static_cast<OndaCompileCmdData*>(inUserData);
    if (!cmdData) {
        Print("ERROR: Onda: invalid compile command payload.\n");
        return false;
    }

    if (!cmdData->path) {
        Print("ERROR: Onda: invalid cmd->path.\n");
        return false;
    }

    std::string filePath = cmdData->path;
    std::error_code ec;
    
    const bool isFile = std::filesystem::exists(filePath, ec)
            && std::filesystem::is_regular_file(filePath, ec);
    
    if (!isFile) {
        Print("ERROR: Onda: failed to read source file '%s'.\n", filePath.c_str());
        return true;
    }

    onda_compile_options_t compileOptions{};
    compileOptions.fast_math = 0;
    compileOptions.sample_rate = static_cast<float>(world->mSampleRate);
    compileOptions.block_size = world->mBufLength;

    onda_diag_t diag{};
    onda_program_t* program = onda_compile_file(filePath.c_str(), &compileOptions, &diag);

    if (!program) {
        const char* message = diag.message ? diag.message : "unknown compile error";
        Print("ERROR: Onda: compile failed (%d:%d): %s\n", diag.line, diag.column, message);
        std::snprintf(cmdData->replyMsg, sizeof(cmdData->replyMsg), "_onda/%d/_fail", cmdData->hash);
        return true;
    }

    auto* compiled = new CompiledProgram();
    compiled->program = program;

    std::string metaError;
    if (!buildProgramMetadata(*compiled, metaError)) {
        Print("ERROR: Onda: %s\n", metaError.c_str());
        destroyCompiledProgram(compiled);
        std::snprintf(cmdData->replyMsg, sizeof(cmdData->replyMsg), "_onda/%d/_fail", cmdData->hash);
        return true;
    }

    const int preallocateCount = (cmdData->numAllocate > 0) ? cmdData->numAllocate : 1;
    compiled->instances.reserve(static_cast<size_t>(preallocateCount));
    for (int i = 0; i < preallocateCount; ++i) {
        onda_diag_t instanceDiag{};
        onda_instance_t* instance = onda_instance_create(
            compiled->program,
            compiled->requiredInputChannels,
            (compiled->requiredOutputChannels > 0) ? compiled->requiredOutputChannels : 1,
            &instanceDiag);

        if (!instance) {
            const char* msg = instanceDiag.message ? instanceDiag.message : "unknown instance creation error";
            Print("ERROR: Onda: failed to preallocate instance %d/%d for hash %d: %s\n", i + 1, preallocateCount, cmdData->hash, msg);
            destroyCompiledProgram(compiled);
            std::snprintf(cmdData->replyMsg, sizeof(cmdData->replyMsg), "_onda/%d/_fail", cmdData->hash);
            return true;
        }

        CompiledProgram::PreallocatedInstance slot;
        slot.instance = instance;
        slot.unit = nullptr;
        compiled->instances.push_back(slot);
    }

    std::string reply;
    writeReply(reply, cmdData->hash, *compiled);

    if (reply.size() > static_cast<size_t>(kMaxReplySize)) {
        Print("ERROR: Onda: reply payload too large (%d).\n", static_cast<int>(reply.size()));
        destroyCompiledProgram(compiled);
        std::snprintf(cmdData->replyMsg, sizeof(cmdData->replyMsg), "_onda/%d/_fail", cmdData->hash);
        return true;
    }

    std::strncpy(cmdData->replyMsg, reply.c_str(), sizeof(cmdData->replyMsg) - 1);
    cmdData->replyMsg[sizeof(cmdData->replyMsg) - 1] = '\0';
    cmdData->newProgram = compiled;

    Print(
        "Onda: compiled hash %d (%d inputs, %d flattened outputs, %d preallocated instances).\n",
        cmdData->hash,
        static_cast<int>(compiled->inputs.size()),
        compiled->outputChannels,
        static_cast<int>(compiled->instances.size()));

    return true;
}

void ondaCompile(World* inWorld, void* /*inUserData*/, struct sc_msg_iter* args, void* replyAddr) {
    auto* cmdData = static_cast<OndaCompileCmdData*>(RTAlloc(inWorld, sizeof(OndaCompileCmdData)));
    if (!cmdData) {
        Print("ERROR: Onda: failed to allocate compile command data.\n");
        return;
    }

    std::memset(cmdData, 0, sizeof(OndaCompileCmdData));

    cmdData->hash = args->geti();
    cmdData->numAllocate = args->geti();
    
    const char* path = args->gets();

    if (!path) {
        return;
    }

    const size_t len = std::strlen(path);

    cmdData->path = static_cast<char*>(RTAlloc(inWorld, len + 1));
    if (!cmdData->path) {
        Print("ERROR: Onda: failed to allocate path.\n");
        RTFree(inWorld, cmdData);
        return;
    }

    std::memcpy(cmdData->path, path, len + 1);

    DoAsynchronousCommand(
        inWorld,
        replyAddr,
        &cmdData->replyMsg[0],
        cmdData,
        static_cast<AsyncStageFn>(ondaCompileStage2),
        static_cast<AsyncStageFn>(ondaCompileStage3),
        static_cast<AsyncStageFn>(ondaCompileStage4),
        static_cast<AsyncFreeFn>(ondaCompileCleanup),
        0,
        nullptr);
}

struct OndaFreeCmdData {
    int hash = 0;
    CompiledProgram* programToDelete = nullptr;
};

// RT cleanup stage (audio thread): release command payload allocated with RTAlloc.
void ondaFreeCleanup(World* world, void* inUserData) {
    auto* cmdData = static_cast<OndaFreeCmdData*>(inUserData);
    if (cmdData) {
        RTFree(world, cmdData);
    }
}

// NRT stage 4 (worker thread): destroy removed compiled program.
bool ondaFreeStage4(World* /*world*/, void* inUserData) {
    auto* cmdData = static_cast<OndaFreeCmdData*>(inUserData);
    if (cmdData && cmdData->programToDelete) {
        destroyCompiledProgram(cmdData->programToDelete);
        cmdData->programToDelete = nullptr;
    }

    return true;
}

// RT stage 3 (audio thread): remove program from registry and detach live units.
bool ondaFreeStage3(World* /*world*/, void* inUserData) {
    auto* cmdData = static_cast<OndaFreeCmdData*>(inUserData);
    if (!cmdData) {
        return true;
    }

    cmdData->programToDelete = Onda::removeProgram(cmdData->hash);
    if (cmdData->programToDelete) {
        for (const auto& slot : cmdData->programToDelete->instances) {
            Onda* unit = slot.unit;
            if (unit) {
                unit->handleFree(cmdData->hash);
            }
        }
    }
    return true;
}

// NRT stage 2 (worker thread): reserved for pre-RT work (currently no-op).
bool ondaFreeStage2(World* /*world*/, void* /*inUserData*/) {
    return true;
}

void ondaFree(World* inWorld, void* /*inUserData*/, struct sc_msg_iter* args, void* replyAddr) {
    auto* cmdData = static_cast<OndaFreeCmdData*>(RTAlloc(inWorld, sizeof(OndaFreeCmdData)));
    if (!cmdData) {
        return;
    }

    std::memset(cmdData, 0, sizeof(OndaFreeCmdData));
    cmdData->hash = args->geti();

    DoAsynchronousCommand(
        inWorld,
        replyAddr,
        nullptr,
        cmdData,
        static_cast<AsyncStageFn>(ondaFreeStage2),
        static_cast<AsyncStageFn>(ondaFreeStage3),
        static_cast<AsyncStageFn>(ondaFreeStage4),
        static_cast<AsyncFreeFn>(ondaFreeCleanup),
        0,
        nullptr);
}

} // namespace

CompiledProgram* Onda::getProgramByHash(int hash) {
    for (const auto& entry : patchStorage) {
        if (entry.active && entry.hash == hash) {
            return entry.program;
        }
    }

    return nullptr;
}

CompiledProgram* Onda::insertOrUpdateProgram(int hash, CompiledProgram* program) {
    for (auto& entry : patchStorage) {
        if (entry.active && entry.hash == hash) {
            CompiledProgram* old = entry.program;
            entry.program = program;
            return old;
        }
    }

    for (auto& entry : patchStorage) {
        if (!entry.active) {
            entry.hash = hash;
            entry.program = program;
            entry.active = true;
            return nullptr;
        }
    }

    Print(
        "ERROR: Onda: patch storage is full (%d). Increase size in PluginLoad.\n",
        static_cast<int>(patchStorage.size()));

    return program;
}

CompiledProgram* Onda::removeProgram(int hash) {
    for (auto& entry : patchStorage) {
        if (entry.active && entry.hash == hash) {
            entry.active = false;
            CompiledProgram* old = entry.program;
            entry.program = nullptr;
            return old;
        }
    }

    return nullptr;
}

bool Onda::bindProgram(CompiledProgram* program, bool isHotSwap) {
    if (!program || !program->program) {
        return false;
    }

    onda_instance_t* claimedInstance = nullptr;
    int claimedIndex = -1;
    if (!claimInstance(program, claimedInstance, claimedIndex)) {
        onda_diag_t instanceDiag{};
        claimedInstance = onda_instance_create(
            program->program,
            program->requiredInputChannels,
            (program->requiredOutputChannels > 0) ? program->requiredOutputChannels : 1,
            &instanceDiag);

        if (!claimedInstance) {
            const char* msg = instanceDiag.message ? instanceDiag.message : "unknown instance creation error";
            Print(
                "ERROR: Onda (%d): no preallocated instances available and fallback instance creation failed for hash %d: %s\n",
                mHash,
                mHash,
                msg);
            return false;
        }

        CompiledProgram::PreallocatedInstance slot;
        slot.instance = claimedInstance;
        slot.unit = this;
        program->instances.push_back(slot);
        claimedIndex = static_cast<int>(program->instances.size()) - 1;

        Print(
            "WARNING: Onda (%d): preallocated instances exhausted for hash %d. Created fallback runtime instance.\n",
            mHash,
            mHash);
    }

    const onda_instance_t* expected = (claimedIndex >= 0 && claimedIndex < static_cast<int>(program->instances.size()))
        ? program->instances[static_cast<size_t>(claimedIndex)].instance
        : nullptr;

    if (expected != claimedInstance || !claimedInstance) {
        Print("ERROR: Onda: invalid runtime instance claim state.\n");
        if (claimedIndex >= 0 && claimedIndex < static_cast<int>(program->instances.size())) {
            program->instances[static_cast<size_t>(claimedIndex)].unit = nullptr;
        }
        return false;
    }

    if (onda_reset_instance_state(claimedInstance) != 0) {
        Print("ERROR: Onda: failed to reset runtime instance for hash %d.\n", mHash);
        program->instances[static_cast<size_t>(claimedIndex)].unit = nullptr;
        return false;
    }

    if (!allocateRtState(program)) {
        program->instances[static_cast<size_t>(claimedIndex)].unit = nullptr;
        return false;
    }

    releaseInstance();

    mProgram = program;
    mInstance = claimedInstance;
    mInstanceSlot = claimedIndex;

    mCalcFunc = make_calc_function<Onda, &Onda::next>();

    if (isHotSwap) {
        Print("Onda: hot-swapped hash %d.\n", mHash);
    }

    return true;
}

bool Onda::bindLatestProgram(bool isHotSwap) {
    CompiledProgram* latest = getProgramByHash(mHash);
    if (!latest) {
        return false;
    }

    if (latest == mProgram && mInstance) {
        return true;
    }

    return bindProgram(latest, isHotSwap);
}

bool Onda::claimInstance(CompiledProgram* program, onda_instance_t*& outInstance, int& outIndex) {
    outInstance = nullptr;
    outIndex = -1;

    if (!program) {
        return false;
    }

    for (int i = 0; i < static_cast<int>(program->instances.size()); ++i) {
        auto& slot = program->instances[static_cast<size_t>(i)];
        if (!slot.instance || slot.unit != nullptr) {
            continue;
        }

        slot.unit = this;
        outInstance = slot.instance;
        outIndex = i;
        return true;
    }

    return false;
}

bool Onda::allocateRtState(CompiledProgram* program) {
    const OndaInputDescriptor* newBoundInputs = program->inputs.data();
    const int newBoundInputCount = static_cast<int>(program->inputs.size());
    const OndaInputDescriptor* newBoundOutputs = program->outputs.data();
    const int newBoundOutputCount = static_cast<int>(program->outputs.size());

    int* newScInputSlotByDesc = nullptr;
    RuntimeInputState* newRuntimeInputs = nullptr;
    RuntimeBufferState* newRuntimeBuffers = nullptr;
    RuntimeOutputState* newRuntimeOutputs = nullptr;
    uint8_t* newOutputScratchBlock = nullptr;
    bool newNeedsOutputCopy = false;
    int* newAudioInputDescIndices = nullptr;
    int newAudioInputDescCount = 0;
    int* newParamDescIndices = nullptr;
    int newParamDescCount = 0;
    int* newEventDescIndices = nullptr;
    int newEventDescCount = 0;
    int* newBufferDescIndices = nullptr;
    int newBufferDescCount = 0;

#if SUPERNOVA
    BufferLockState* newBufferLocks = nullptr;
    int newBufferLockCapacity = 0;
    int newBufferLockCount = 0;
#endif

    if (newBoundInputCount > 0) {
        newScInputSlotByDesc = static_cast<int*>(RTAlloc(mWorld, static_cast<size_t>(newBoundInputCount) * sizeof(int)));
        if (!newScInputSlotByDesc) {
            Print("ERROR: Onda: failed to allocate input slot map.\n");
            return false;
        }

        const int maxScSlots = numInputs() - 1;
        for (int i = 0; i < newBoundInputCount; ++i) {
            newScInputSlotByDesc[i] = (i < maxScSlots) ? (i + 1) : -1;
        }

        if (newBoundInputCount != maxScSlots) {
            Print(
                "ERROR: Onda (%d): Program requires exactly %d SC inputs but UGen has %d. Relaunch the UGen with the new IO shape.\n",
                mHash,
                newBoundInputCount,
                maxScSlots);
            RTFree(mWorld, newScInputSlotByDesc);
            return false;
        }

        newRuntimeInputs = static_cast<RuntimeInputState*>(RTAlloc(mWorld, static_cast<size_t>(newBoundInputCount) * sizeof(RuntimeInputState)));
        if (!newRuntimeInputs) {
            Print("ERROR: Onda: failed to allocate input runtime state.\n");
            RTFree(mWorld, newScInputSlotByDesc);
            return false;
        }
        std::memset(newRuntimeInputs, 0, static_cast<size_t>(newBoundInputCount) * sizeof(RuntimeInputState));

        newRuntimeBuffers = static_cast<RuntimeBufferState*>(RTAlloc(mWorld, static_cast<size_t>(newBoundInputCount) * sizeof(RuntimeBufferState)));
        if (!newRuntimeBuffers) {
            Print("ERROR: Onda: failed to allocate buffer runtime state.\n");
            RTFree(mWorld, newRuntimeInputs);
            RTFree(mWorld, newScInputSlotByDesc);
            return false;
        }
        std::memset(newRuntimeBuffers, 0, static_cast<size_t>(newBoundInputCount) * sizeof(RuntimeBufferState));
    }

    if (newBoundOutputCount > 0) {
        newRuntimeOutputs = static_cast<RuntimeOutputState*>(RTAlloc(mWorld, static_cast<size_t>(newBoundOutputCount) * sizeof(RuntimeOutputState)));
        if (!newRuntimeOutputs) {
            Print("ERROR: Onda: failed to allocate output runtime state.\n");
            if (newScInputSlotByDesc) {
                RTFree(mWorld, newScInputSlotByDesc);
            }
            if (newRuntimeInputs) {
                RTFree(mWorld, newRuntimeInputs);
            }
            if (newRuntimeBuffers) {
                RTFree(mWorld, newRuntimeBuffers);
            }
            return false;
        }
        std::memset(newRuntimeOutputs, 0, static_cast<size_t>(newBoundOutputCount) * sizeof(RuntimeOutputState));

        int usedScOuts = 0;
        size_t totalScratchBytes = 0;
        for (int i = 0; i < newBoundOutputCount; ++i) {
            const auto& outDesc = newBoundOutputs[i];
            auto& state = newRuntimeOutputs[i];

            const bool canMap = (usedScOuts + outDesc.arrayLen) <= numOutputs();
            state.mapped = canMap;
            state.scOffset = canMap ? usedScOuts : -1;
            state.directBind = canMap && outDesc.arrayLen == 1;
            state.scratchBytes = state.directBind
                ? 0
                : static_cast<int>(sizeof(float) * static_cast<size_t>(outDesc.arrayLen) * static_cast<size_t>(bufferSize()));

            if (state.scratchBytes > 0) {
                totalScratchBytes += static_cast<size_t>(state.scratchBytes);
            }

            if (canMap) {
                usedScOuts += outDesc.arrayLen;
                if (!state.directBind) {
                    newNeedsOutputCopy = true;
                }
            }
        }

        if (program->requiredOutputChannels != numOutputs()) {
            Print(
                "ERROR: Onda (%d): Program requires exactly %d output channels but UGen has %d. Relaunch the UGen with the new IO shape.\n",
                mHash,
                program->requiredOutputChannels,
                numOutputs());
            if (newScInputSlotByDesc) {
                RTFree(mWorld, newScInputSlotByDesc);
            }
            if (newRuntimeInputs) {
                RTFree(mWorld, newRuntimeInputs);
            }
            if (newRuntimeBuffers) {
                RTFree(mWorld, newRuntimeBuffers);
            }
            if (newRuntimeOutputs) {
                RTFree(mWorld, newRuntimeOutputs);
            }
            return false;
        }

        if (usedScOuts != program->requiredOutputChannels) {
            Print(
                "ERROR: Onda (%d): internal output mapping mismatch (%d mapped, %d required).\n",
                mHash,
                usedScOuts,
                program->requiredOutputChannels);
            if (newScInputSlotByDesc) {
                RTFree(mWorld, newScInputSlotByDesc);
            }
            if (newRuntimeInputs) {
                RTFree(mWorld, newRuntimeInputs);
            }
            if (newRuntimeBuffers) {
                RTFree(mWorld, newRuntimeBuffers);
            }
            if (newRuntimeOutputs) {
                RTFree(mWorld, newRuntimeOutputs);
            }
            return false;
        }

        if (totalScratchBytes > 0) {
            newOutputScratchBlock = static_cast<uint8_t*>(RTAlloc(mWorld, totalScratchBytes));
            if (!newOutputScratchBlock) {
                Print("ERROR: Onda: failed to allocate output scratch block.\n");
                if (newScInputSlotByDesc) {
                    RTFree(mWorld, newScInputSlotByDesc);
                }
                if (newRuntimeInputs) {
                    RTFree(mWorld, newRuntimeInputs);
                }
                if (newRuntimeBuffers) {
                    RTFree(mWorld, newRuntimeBuffers);
                }
                if (newRuntimeOutputs) {
                    RTFree(mWorld, newRuntimeOutputs);
                }
                return false;
            }
            std::memset(newOutputScratchBlock, 0, totalScratchBytes);

            uint8_t* scratchCursor = newOutputScratchBlock;
            for (int i = 0; i < newBoundOutputCount; ++i) {
                auto& state = newRuntimeOutputs[i];
                if (state.scratchBytes > 0) {
                    state.scratch = scratchCursor;
                    scratchCursor += state.scratchBytes;
                }
            }
        }
    }

#if SUPERNOVA
    int bufferInputCount = 0;
    for (int i = 0; i < newBoundInputCount; ++i) {
        if (newBoundInputs[i].kind == OndaInputKind::Buffer) {
            ++bufferInputCount;
        }
    }

    newBufferLockCapacity = bufferInputCount;
    newBufferLockCount = 0;
    if (newBufferLockCapacity > 0) {
        newBufferLocks = static_cast<BufferLockState*>(RTAlloc(mWorld, static_cast<size_t>(newBufferLockCapacity) * sizeof(BufferLockState)));
        if (!newBufferLocks) {
            Print("ERROR: Onda: failed to allocate buffer lock state.\n");
            if (newScInputSlotByDesc) {
                RTFree(mWorld, newScInputSlotByDesc);
            }
            if (newRuntimeInputs) {
                RTFree(mWorld, newRuntimeInputs);
            }
            if (newRuntimeBuffers) {
                RTFree(mWorld, newRuntimeBuffers);
            }
            if (newRuntimeOutputs) {
                RTFree(mWorld, newRuntimeOutputs);
            }
            if (newOutputScratchBlock) {
                RTFree(mWorld, newOutputScratchBlock);
            }
            return false;
        }
        std::memset(newBufferLocks, 0, static_cast<size_t>(newBufferLockCapacity) * sizeof(BufferLockState));
    }
#endif

    for (int i = 0; i < newBoundInputCount; ++i) {
        const auto& desc = newBoundInputs[i];
        if (desc.kind == OndaInputKind::Input) {
            ++newAudioInputDescCount;
        } else if (desc.kind == OndaInputKind::Param) {
            ++newParamDescCount;
        } else if (desc.kind == OndaInputKind::Event) {
            ++newEventDescCount;
        } else if (desc.kind == OndaInputKind::Buffer) {
            ++newBufferDescCount;
        }
    }

    auto allocateIndexMap = [&](int count, int*& outPtr, const char* label) -> bool {
        if (count <= 0) {
            outPtr = nullptr;
            return true;
        }

        outPtr = static_cast<int*>(RTAlloc(mWorld, static_cast<size_t>(count) * sizeof(int)));
        if (!outPtr) {
            Print("ERROR: Onda: failed to allocate %s index map.\n", label);
            return false;
        }

        return true;
    };

    if (!allocateIndexMap(newAudioInputDescCount, newAudioInputDescIndices, "audio-input")
        || !allocateIndexMap(newParamDescCount, newParamDescIndices, "param")
        || !allocateIndexMap(newEventDescCount, newEventDescIndices, "event")
        || !allocateIndexMap(newBufferDescCount, newBufferDescIndices, "buffer")) {
        if (newAudioInputDescIndices) {
            RTFree(mWorld, newAudioInputDescIndices);
        }
        if (newParamDescIndices) {
            RTFree(mWorld, newParamDescIndices);
        }
        if (newEventDescIndices) {
            RTFree(mWorld, newEventDescIndices);
        }
        if (newBufferDescIndices) {
            RTFree(mWorld, newBufferDescIndices);
        }
        if (newScInputSlotByDesc) {
            RTFree(mWorld, newScInputSlotByDesc);
        }
        if (newRuntimeInputs) {
            RTFree(mWorld, newRuntimeInputs);
        }
        if (newRuntimeBuffers) {
            RTFree(mWorld, newRuntimeBuffers);
        }
        if (newRuntimeOutputs) {
            RTFree(mWorld, newRuntimeOutputs);
        }
        if (newOutputScratchBlock) {
            RTFree(mWorld, newOutputScratchBlock);
        }
#if SUPERNOVA
        if (newBufferLocks) {
            RTFree(mWorld, newBufferLocks);
        }
#endif
        return false;
    }

    int audioCursor = 0;
    int paramCursor = 0;
    int eventCursor = 0;
    int bufferCursor = 0;
    for (int i = 0; i < newBoundInputCount; ++i) {
        const auto& desc = newBoundInputs[i];
        if (desc.kind == OndaInputKind::Input) {
            newAudioInputDescIndices[audioCursor++] = i;
        } else if (desc.kind == OndaInputKind::Param) {
            newParamDescIndices[paramCursor++] = i;
        } else if (desc.kind == OndaInputKind::Event) {
            newEventDescIndices[eventCursor++] = i;
        } else if (desc.kind == OndaInputKind::Buffer) {
            newBufferDescIndices[bufferCursor++] = i;
        }
    }

    freeRtState();

    mBoundInputs = newBoundInputs;
    mBoundInputCount = newBoundInputCount;
    mBoundOutputs = newBoundOutputs;
    mBoundOutputCount = newBoundOutputCount;
    mAudioInputDescIndices = newAudioInputDescIndices;
    mAudioInputDescCount = newAudioInputDescCount;
    mParamDescIndices = newParamDescIndices;
    mParamDescCount = newParamDescCount;
    mEventDescIndices = newEventDescIndices;
    mEventDescCount = newEventDescCount;
    mBufferDescIndices = newBufferDescIndices;
    mBufferDescCount = newBufferDescCount;
    mScInputSlotByDesc = newScInputSlotByDesc;
    mRuntimeInputs = newRuntimeInputs;
    mRuntimeBuffers = newRuntimeBuffers;
    mRuntimeOutputs = newRuntimeOutputs;
    mOutputScratchBlock = newOutputScratchBlock;
    mNeedsOutputCopy = newNeedsOutputCopy;
#if SUPERNOVA
    mBufferLocks = newBufferLocks;
    mBufferLockCapacity = newBufferLockCapacity;
    mBufferLockCount = newBufferLockCount;
#endif
    mBindingsNeedValidate = false;

    return true;
}

void Onda::freeRtState() {
    if (mAudioInputDescIndices) {
        RTFree(mWorld, mAudioInputDescIndices);
        mAudioInputDescIndices = nullptr;
    }

    if (mParamDescIndices) {
        RTFree(mWorld, mParamDescIndices);
        mParamDescIndices = nullptr;
    }

    if (mEventDescIndices) {
        RTFree(mWorld, mEventDescIndices);
        mEventDescIndices = nullptr;
    }

    if (mBufferDescIndices) {
        RTFree(mWorld, mBufferDescIndices);
        mBufferDescIndices = nullptr;
    }

    if (mScInputSlotByDesc) {
        RTFree(mWorld, mScInputSlotByDesc);
        mScInputSlotByDesc = nullptr;
    }

    if (mRuntimeInputs) {
        RTFree(mWorld, mRuntimeInputs);
        mRuntimeInputs = nullptr;
    }

    if (mRuntimeBuffers) {
        RTFree(mWorld, mRuntimeBuffers);
        mRuntimeBuffers = nullptr;
    }

    if (mRuntimeOutputs) {
        RTFree(mWorld, mRuntimeOutputs);
        mRuntimeOutputs = nullptr;
    }

    if (mOutputScratchBlock) {
        RTFree(mWorld, mOutputScratchBlock);
        mOutputScratchBlock = nullptr;
    }
    mNeedsOutputCopy = false;

#if SUPERNOVA
    mBufferLocksHeld = false;
    if (mBufferLocks) {
        RTFree(mWorld, mBufferLocks);
        mBufferLocks = nullptr;
    }
#endif

    mBoundInputs = nullptr;
    mBoundInputCount = 0;
    mBoundOutputs = nullptr;
    mBoundOutputCount = 0;
    mAudioInputDescCount = 0;
    mParamDescCount = 0;
    mEventDescCount = 0;
    mBufferDescCount = 0;
#if SUPERNOVA
    mBufferLockCapacity = 0;
    mBufferLockCount = 0;
#endif
    mBindingsNeedValidate = false;
}

void Onda::releaseInstance() {
    if (mProgram && mInstanceSlot >= 0 && mInstanceSlot < static_cast<int>(mProgram->instances.size())) {
        auto& slot = mProgram->instances[static_cast<size_t>(mInstanceSlot)];
        if (slot.unit == this) {
            slot.unit = nullptr;
        }
    }

    mInstance = nullptr;
    mInstanceSlot = -1;
}

void Onda::handleHotSwap(int hash, CompiledProgram* program) {
    if (hash != mHash || !program) {
        return;
    }

    if (program == mProgram && mInstance) {
        return;
    }

    if (!bindProgram(program, true)) {
        Print("WARNING: Onda (%d): hot-swap failed. Releasing current instance and silencing unit.\n", mHash);
        releaseInstance();
        freeRtState();
        mProgram = nullptr;
        setSilence();
    }
}

void Onda::handleFree(int hash) {
    if (hash != mHash) {
        return;
    }

    releaseInstance();
    freeRtState();
    mProgram = nullptr;
    setSilence();
}

SndBuf* Onda::resolveSndBufByIndex(int bufIndex) const {
    if (bufIndex < 0) {
        return nullptr;
    }

    World* world = mWorld;
    if (!world || !world->mSndBufs) {
        return nullptr;
    }

    const int globalCount = static_cast<int>(world->mNumSndBufs);
    if (bufIndex < globalCount) {
        return world->mSndBufs + bufIndex;
    }

    Graph* parent = mParent;
    if (!parent || !parent->mLocalSndBufs) {
        return nullptr;
    }

    const int localBufNum = bufIndex - globalCount;
    if (localBufNum < 0 || localBufNum > parent->localBufNum) {
        return nullptr;
    }

    return parent->mLocalSndBufs + localBufNum;
}

bool Onda::isValidBufferBinding(const SndBuf* buf, const OndaInputDescriptor& desc) const {
    if (!buf || !buf->data) {
        return false;
    }

    if (buf->frames <= 0 || buf->channels <= 0) {
        return false;
    }

    switch (desc.bufferChannelsKind) {
        case ONDA_BUFFER_CHANNELS_MONO:
            return buf->channels == 1;
        case ONDA_BUFFER_CHANNELS_STATIC:
            return buf->channels == desc.bufferChannelsStatic;
        case ONDA_BUFFER_CHANNELS_DYNAMIC:
            return true;
        default:
            return false;
    }
}

#if SUPERNOVA
void Onda::acquireBufferLocks() {
    if (mBufferLocksHeld || mBufferLockCount <= 0 || !mBufferLocks) {
        return;
    }

    for (int i = 0; i < mBufferLockCount; ++i) {
        const auto& lock = mBufferLocks[i];
        if (!lock.buf || lock.buf->isLocal) {
            continue;
        }

        if (lock.exclusive) {
            lock.buf->lock.lock();
        } else {
            lock.buf->lock.lock_shared();
        }
    }

    mBufferLocksHeld = true;
}

void Onda::releaseBufferLocks() {
    if (!mBufferLocksHeld || mBufferLockCount <= 0 || !mBufferLocks) {
        return;
    }

    for (int i = mBufferLockCount - 1; i >= 0; --i) {
        const auto& lock = mBufferLocks[i];
        if (!lock.buf || lock.buf->isLocal) {
            continue;
        }

        if (lock.exclusive) {
            lock.buf->lock.unlock();
        } else {
            lock.buf->lock.unlock_shared();
        }
    }

    mBufferLocksHeld = false;
}

bool Onda::addOrUpgradeBufferLock(SndBuf* buf, bool exclusive) {
    if (!buf || buf->isLocal) {
        return true;
    }

    const std::uintptr_t key = reinterpret_cast<std::uintptr_t>(buf);
    int lo = 0;
    int hi = mBufferLockCount;
    while (lo < hi) {
        const int mid = lo + ((hi - lo) / 2);
        const std::uintptr_t midKey = reinterpret_cast<std::uintptr_t>(mBufferLocks[mid].buf);
        if (midKey < key) {
            lo = mid + 1;
        } else {
            hi = mid;
        }
    }

    const int insertPos = lo;
    if (insertPos < mBufferLockCount && mBufferLocks[insertPos].buf == buf) {
        mBufferLocks[insertPos].exclusive = mBufferLocks[insertPos].exclusive || exclusive;
        return true;
    }

    if (mBufferLockCount >= mBufferLockCapacity) {
        return false;
    }

    if (insertPos < mBufferLockCount) {
        const size_t moveCount = static_cast<size_t>(mBufferLockCount - insertPos);
        std::memmove(
            mBufferLocks + insertPos + 1,
            mBufferLocks + insertPos,
            moveCount * sizeof(BufferLockState));
    }

    auto& lockState = mBufferLocks[insertPos];
    lockState.buf = buf;
    lockState.exclusive = exclusive;
    ++mBufferLockCount;
    return true;
}
#endif

Onda::BufferPrepResult Onda::prepareBuffers() {
#if SUPERNOVA
    mBufferLockCount = 0;
    mBufferLocksHeld = false;

    for (int n = 0; n < mBufferDescCount; ++n) {
        const int i = mBufferDescIndices[n];
        const auto& desc = mBoundInputs[i];

        const int scSlot = mScInputSlotByDesc[i];
        const float fallback = desc.hasInit ? desc.init : 0.0f;
        const float rawBufNum = (scSlot > 0) ? in0(scSlot) : fallback;
        const int bufIndex = static_cast<int>(rawBufNum);
        SndBuf* buf = resolveSndBufByIndex(bufIndex);

        if (!addOrUpgradeBufferLock(buf, desc.bufferMayWrite)) {
            Print("ERROR: Onda (%d): buffer lock state capacity exceeded.\n", mHash);
            return BufferPrepResult::Fatal;
        }
    }

    acquireBufferLocks();
#endif

    bool allValid = true;
    
    for (int n = 0; n < mBufferDescCount; ++n) {
        const int i = mBufferDescIndices[n];
        const auto& desc = mBoundInputs[i];

        const int scSlot = mScInputSlotByDesc[i];
        const float fallback = desc.hasInit ? desc.init : 0.0f;
        const float rawBufNum = (scSlot > 0) ? in0(scSlot) : fallback;
        const int bufIndex = static_cast<int>(rawBufNum);
        SndBuf* buf = resolveSndBufByIndex(bufIndex);
        auto& state = mRuntimeBuffers[i];

        if (!isValidBufferBinding(buf, desc)) {
            allValid = false;
            if (state.bound) {
                if (onda_bind_buffer(mInstance, desc.ondaIndex, nullptr, 0, 0, 0.0f, ONDA_PRIMITIVE_F32) != 0) {
                    Print("ERROR: Onda (%d): failed to unbind invalid buffer '%s'.\n", mHash, desc.name.c_str());
                    return BufferPrepResult::Fatal;
                }
                state.bound = false;
                state.boundPtr = nullptr;
                state.boundBufIndex = -1;
                mBindingsNeedValidate = true;
            }
            continue;
        }

        void* ptr = static_cast<void*>(buf->data);
        const int frames = buf->frames;
        const int channels = buf->channels;
        const float sampleRate = static_cast<float>(buf->samplerate);
        const bool needsRebind = !state.bound
            || state.boundBufIndex != bufIndex
            || state.boundPtr != ptr;

        if (needsRebind) {
            if (onda_bind_buffer(mInstance, desc.ondaIndex, ptr, frames, channels, sampleRate, ONDA_PRIMITIVE_F32) != 0) {
                Print("ERROR: Onda (%d): failed to bind buffer '%s' (bufnum=%d).\n", mHash, desc.name.c_str(), bufIndex);
                return BufferPrepResult::Fatal;
            }
            state.bound = true;
            state.boundPtr = ptr;
            state.boundBufIndex = bufIndex;
            mBindingsNeedValidate = true;
        }
    }

    return allValid ? BufferPrepResult::Ok : BufferPrepResult::Invalid;
}

bool Onda::prepareInputs(int nSamples) {
    for (int n = 0; n < mAudioInputDescCount; ++n) {
        const int i = mAudioInputDescIndices[n];
        const auto& desc = mBoundInputs[i];

        const int scSlot = mScInputSlotByDesc[i];
        const float* src = (scSlot > 0) ? in(scSlot) : nullptr;

        if (!src) {
            Print(
                "ERROR: Onda (%d): missing bound audio input '%s'. Relaunch the UGen with the new IO shape.\n",
                mHash,
                desc.name.c_str());
            return false;
        }

        const void* bindPtr = src;
        const int bytes = static_cast<int>(sizeof(float) * static_cast<size_t>(nSamples));
        auto& state = mRuntimeInputs[i];
        const bool needsRebind = !state.bound || state.boundPtr != bindPtr || state.boundBytes != bytes;
        if (needsRebind) {
            if (onda_bind_input(mInstance, desc.ondaIndex, bindPtr, bytes) != 0) {
                Print("ERROR: Onda (%d): failed to bind input '%s'.\n", mHash, desc.name.c_str());
                return false;
            }
            state.bound = true;
            state.boundPtr = bindPtr;
            state.boundBytes = bytes;
            mBindingsNeedValidate = true;
        }
    }

    return true;
}

bool Onda::prepareParamsAndEvents() {
    for (int n = 0; n < mParamDescCount; ++n) {
        const int i = mParamDescIndices[n];
        const auto& desc = mBoundInputs[i];
        const int scSlot = mScInputSlotByDesc[i];
        const float fallback = desc.hasInit ? desc.init : 0.0f;
        const float value = (scSlot > 0) ? in0(scSlot) : fallback;

        if (onda_set_param_by_index(mInstance, desc.ondaIndex, &value, static_cast<int>(sizeof(float))) != 0) {
            Print("ERROR: Onda (%d): failed to set param '%s'.\n", mHash, desc.name.c_str());
            return false;
        }
    }

    for (int n = 0; n < mEventDescCount; ++n) {
        const int i = mEventDescIndices[n];
        const auto& desc = mBoundInputs[i];
        const int scSlot = mScInputSlotByDesc[i];
        const float fallback = desc.hasInit ? desc.init : 0.0f;
        const float value = (scSlot > 0) ? in0(scSlot) : fallback;

        if (onda_trigger_event_by_index(mInstance, desc.ondaIndex, &value, static_cast<int>(sizeof(float))) != 0) {
            Print("ERROR: Onda (%d): failed to trigger event '%s'.\n", mHash, desc.name.c_str());
            return false;
        }
    }

    return true;
}

bool Onda::prepareOutputs(int nSamples) {
    for (int i = 0; i < mBoundOutputCount; ++i) {
        const auto& outDesc = mBoundOutputs[i];
        auto& outState = mRuntimeOutputs[i];

        void* ptr = nullptr;
        int bytes = outDesc.elemBytes * outDesc.arrayLen * nSamples;

        if (outState.directBind) {
            ptr = out(outState.scOffset);
            bytes = static_cast<int>(sizeof(float) * static_cast<size_t>(nSamples));
        } else {
            ptr = outState.scratch;
        }

        const bool needsRebind = !outState.bound || outState.boundPtr != ptr || outState.boundBytes != bytes;
        if (needsRebind) {
            if (onda_bind_output(mInstance, outDesc.ondaIndex, ptr, bytes) != 0) {
                Print("ERROR: Onda (%d): failed to bind output '%s'.\n", mHash, outDesc.name.c_str());
                return false;
            }
            outState.bound = true;
            outState.boundPtr = ptr;
            outState.boundBytes = bytes;
            mBindingsNeedValidate = true;
        }
    }

    return true;
}

void Onda::copyOutputsToSC(int nSamples) {
    for (int i = 0; i < mBoundOutputCount; ++i) {
        const auto& outDesc = mBoundOutputs[i];
        auto& outState = mRuntimeOutputs[i];

        if (!outState.mapped || outState.directBind) {
            continue;
        }

        const uint8_t* base = outState.scratch;
        for (int c = 0; c < outDesc.arrayLen; ++c) {
            float* dst = out(outState.scOffset + c);
            const uint8_t* srcChan = base + (static_cast<size_t>(c) * static_cast<size_t>(nSamples) * static_cast<size_t>(outDesc.elemBytes));
            std::memcpy(dst, srcChan, sizeof(float) * static_cast<size_t>(nSamples));
        }
    }
}

void Onda::silenceBlockOutputs(int nSamples) {
    for (int i = 0; i < numOutputs(); ++i) {
        std::memset(out(i), 0, sizeof(float) * static_cast<size_t>(nSamples));
    }
}

Onda::ProcessAudioResult Onda::processAudio(int nSamples) {
    const BufferPrepResult bufferPrep = prepareBuffers();
    if (bufferPrep == BufferPrepResult::Fatal) {
#if SUPERNOVA
        releaseBufferLocks();
#endif
        return ProcessAudioResult::Fatal;
    }

    if (bufferPrep == BufferPrepResult::Invalid) {
#if SUPERNOVA
        releaseBufferLocks();
#endif
        silenceBlockOutputs(nSamples);
        return ProcessAudioResult::SoftSilence;
    }

    if (!prepareInputs(nSamples)) {
#if SUPERNOVA
        releaseBufferLocks();
#endif
        return ProcessAudioResult::Fatal;
    }

    if (!prepareParamsAndEvents()) {
#if SUPERNOVA
        releaseBufferLocks();
#endif
        return ProcessAudioResult::Fatal;
    }

    if (!prepareOutputs(nSamples)) {
#if SUPERNOVA
        releaseBufferLocks();
#endif
        return ProcessAudioResult::Fatal;
    }

    if (mBindingsNeedValidate) {
        if (onda_validate_bindings(mInstance) != 0) {
            Print("ERROR: Onda (%d): binding validation failed before unchecked process.\n", mHash);
#if SUPERNOVA
            releaseBufferLocks();
#endif
            return ProcessAudioResult::Fatal;
        }
        mBindingsNeedValidate = false;
    }

    const int processResult = onda_process_unchecked(mInstance);
    
#if SUPERNOVA
    releaseBufferLocks();
#endif

    if (processResult != 0) {
        Print("ERROR: Onda (%d): onda_process_unchecked failed.\n", mHash);
        return ProcessAudioResult::Fatal;
    }

    if (mNeedsOutputCopy) {
        copyOutputsToSC(nSamples);
    }
    
    return ProcessAudioResult::Ok;
}

Onda::Onda() {
    mHash = static_cast<int>(in0(0));

    if (!bindLatestProgram(false)) {
        setSilence();
    }
}

Onda::~Onda() {
    releaseInstance();
    freeRtState();
}

void Onda::setSilence() {
    mCalcFunc = make_calc_function<Onda, &Onda::nextSilence>();
}

void Onda::next(int nSamples) {
    if (!mInstance) {
        setSilence();
        return;
    }

    const ProcessAudioResult result = processAudio(nSamples);
    if (result == ProcessAudioResult::Fatal) {
        setSilence();
    }
}

void Onda::nextSilence(int nSamples) {
    silenceBlockOutputs(nSamples);
}

PluginLoad(OndaUGens) {
    ft = inTable;
    registerUnit<Onda>(ft, "Onda", true); // Onda assumes no aliasing between buffers

    Onda::patchStorage.resize(kPatchStorageCapacity);

    ft->fDefinePlugInCmd("onda_compile", ondaCompile, nullptr);
    ft->fDefinePlugInCmd("onda_free", ondaFree, nullptr);
}

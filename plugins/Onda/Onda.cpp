
#include "Onda.hpp"

#include <algorithm>
#include <array>
#include <cctype>
#include <cstdarg>
#include <cinttypes>
#include <cmath>
#include <cstdio>
#include <cstring>
#include <filesystem>
#include <limits>
#include <memory>
#include <new>
#include <sstream>
#include <string>
#include <utility>
#include <vector>

static InterfaceTable* ft;

std::vector<PatchEntry> Onda::patchStorage;

namespace {

constexpr int kMaxReplySize = 4096;
constexpr int kMaxDefinitionId = 65536;
constexpr size_t kMaxPathBytes = 16384;
constexpr uint32_t kDelegateBatchBytes = 8u * 1024u;
constexpr size_t kDelegateFormatBytes = 8u * 1024u;
// Print delivery is diagnostic and must not dominate per-UGen RT memory. Onda drops complete
// records once this bounded batch is full and reports the number through overflow_count.
constexpr uint32_t kPrintBatchBytes = 1u * 1024u;
constexpr size_t kPrintFormatBytes = 2u * 1024u;

void* ondaRtAlloc(void* context, size_t size, size_t align) {
    auto* world = static_cast<World*>(context);
    if (!world) {
        return nullptr;
    }

    void* allocation = RTAlloc(world, size);
    if (allocation
        && align > 1
        && reinterpret_cast<std::uintptr_t>(allocation) % align != 0) {
        RTFree(world, allocation);
        return nullptr;
    }
    return allocation;
}

void ondaRtFree(void* context, void* pointer, size_t /*size*/, size_t /*align*/) {
    if (context && pointer) {
        RTFree(static_cast<World*>(context), pointer);
    }
}

int primitiveByteSize(int elemType) {
    switch (elemType) {
        case ONDA_PRIMITIVE_F32:
        case ONDA_PRIMITIVE_I32:
            return 4;
        case ONDA_PRIMITIVE_F64:
        case ONDA_PRIMITIVE_I64:
            return 8;
        case ONDA_PRIMITIVE_BOOL:
            return 1;
        default:
            return -1;
    }
}

template <typename T>
T integerFromControl(float value) {
    if (!std::isfinite(value)) {
        if (value > 0.0f) {
            return std::numeric_limits<T>::max();
        }
        if (value < 0.0f) {
            return std::numeric_limits<T>::lowest();
        }
        return 0;
    }

    if (value >= static_cast<float>(std::numeric_limits<T>::max())) {
        return std::numeric_limits<T>::max();
    }
    if (value <= static_cast<float>(std::numeric_limits<T>::lowest())) {
        return std::numeric_limits<T>::lowest();
    }
    return static_cast<T>(value);
}

bool packControlPrimitive(
    float value,
    int elemType,
    std::array<uint8_t, sizeof(double)>& payload) {
    payload.fill(0);
    switch (elemType) {
        case ONDA_PRIMITIVE_F32:
            std::memcpy(payload.data(), &value, sizeof(value));
            return true;
        case ONDA_PRIMITIVE_F64: {
            const double converted = static_cast<double>(value);
            std::memcpy(payload.data(), &converted, sizeof(converted));
            return true;
        }
        case ONDA_PRIMITIVE_I32: {
            const int32_t converted = integerFromControl<int32_t>(value);
            std::memcpy(payload.data(), &converted, sizeof(converted));
            return true;
        }
        case ONDA_PRIMITIVE_I64: {
            const int64_t converted = integerFromControl<int64_t>(value);
            std::memcpy(payload.data(), &converted, sizeof(converted));
            return true;
        }
        case ONDA_PRIMITIVE_BOOL:
            payload[0] = value >= 0.5f ? 1 : 0;
            return true;
        default:
            return false;
    }
}

#if defined(__GNUC__) || defined(__clang__)
__attribute__((format(printf, 4, 5)))
#endif
bool appendFormatted(
    char* storage,
    size_t capacity,
    size_t& length,
    const char* format,
    ...) {
    if (!storage || capacity == 0 || length >= capacity) {
        return false;
    }

    std::va_list args;
    va_start(args, format);
    const int written = std::vsnprintf(storage + length, capacity - length, format, args);
    va_end(args);
    if (written < 0 || static_cast<size_t>(written) >= capacity - length) {
        return false;
    }

    length += static_cast<size_t>(written);
    return true;
}

template <typename T>
bool readPackedValue(const uint8_t*& cursor, const uint8_t* end, T& value) {
    if (!cursor || !end || cursor > end || static_cast<size_t>(end - cursor) < sizeof(T)) {
        return false;
    }
    std::memcpy(&value, cursor, sizeof(T));
    cursor += sizeof(T);
    return true;
}

bool appendPackedPrimitive(
    int elemType,
    const uint8_t*& cursor,
    const uint8_t* end,
    char* storage,
    size_t capacity,
    size_t& length,
    bool& payloadValid) {
    switch (elemType) {
        case ONDA_PRIMITIVE_F32: {
            float value = 0.0f;
            if (!readPackedValue(cursor, end, value)) {
                payloadValid = false;
                return false;
            }
            return appendFormatted(storage, capacity, length, "%.9g", static_cast<double>(value));
        }
        case ONDA_PRIMITIVE_F64: {
            double value = 0.0;
            if (!readPackedValue(cursor, end, value)) {
                payloadValid = false;
                return false;
            }
            return appendFormatted(storage, capacity, length, "%.17g", value);
        }
        case ONDA_PRIMITIVE_I32: {
            int32_t value = 0;
            if (!readPackedValue(cursor, end, value)) {
                payloadValid = false;
                return false;
            }
            return appendFormatted(storage, capacity, length, "%" PRId32, value);
        }
        case ONDA_PRIMITIVE_I64: {
            int64_t value = 0;
            if (!readPackedValue(cursor, end, value)) {
                payloadValid = false;
                return false;
            }
            return appendFormatted(storage, capacity, length, "%" PRId64, value);
        }
        case ONDA_PRIMITIVE_BOOL: {
            uint8_t value = 0;
            if (!readPackedValue(cursor, end, value) || value > 1) {
                payloadValid = false;
                return false;
            }
            return appendFormatted(storage, capacity, length, "%s", value ? "true" : "false");
        }
        default:
            payloadValid = false;
            return false;
    }
}

class ScopedOndaDiagnostic {
public:
    ScopedOndaDiagnostic() = default;
    ScopedOndaDiagnostic(const ScopedOndaDiagnostic&) = delete;
    ScopedOndaDiagnostic& operator=(const ScopedOndaDiagnostic&) = delete;

    ~ScopedOndaDiagnostic() { onda_diag_dispose(&mDiagnostic); }

    onda_diag_t* get() { return &mDiagnostic; }
    onda_diag_t* operator->() { return &mDiagnostic; }

private:
    onda_diag_t mDiagnostic{};
};

std::string lowercaseAscii(std::string value) {
    std::transform(value.begin(), value.end(), value.begin(), [](unsigned char c) {
        return static_cast<char>(std::tolower(c));
    });
    return value;
}

bool isProjectPath(const std::filesystem::path& path) {
    return lowercaseAscii(path.extension().u8string()) == ".ondaproject";
}

PatchEntry* patchEntryForDefinition(int definitionId) {
    if (definitionId < 1 || definitionId > static_cast<int>(Onda::patchStorage.size())) {
        return nullptr;
    }

    return &Onda::patchStorage[static_cast<size_t>(definitionId - 1)];
}

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

int scBufferIndex(float value) {
    if (!std::isfinite(value)
        || value < 0.0f
        || std::trunc(value) != value
        || static_cast<double>(value) > static_cast<double>(std::numeric_limits<int>::max())) {
        return -1;
    }

    return static_cast<int>(value);
}

int definitionIdFromControl(float value) {
    if (!std::isfinite(value)
        || std::trunc(value) != value
        || value < 1.0f
        || value > static_cast<float>(kMaxDefinitionId)) {
        return 0;
    }

    return static_cast<int>(value);
}

void destroyCompiledProgram(CompiledProgram* program) {
    if (!program) {
        return;
    }

    if (program->firstUnit) {
        Print("ERROR: Onda: refusing to destroy a compiled program that still has live UGens.\n");
        return;
    }

    if (program->program) {
        onda_program_destroy(program->program);
        program->program = nullptr;
    }

    delete program;
}

bool buildProgramMetadata(
    CompiledProgram& compiled,
    bool compiledFromProject,
    std::string& error) {
    if (!compiled.program) {
        error = "Compiled program handle is null.";
        return false;
    }

    const int bufferCount = onda_buffer_count(compiled.program);
    const int inputCount = onda_input_count(compiled.program);
    const int paramCount = onda_param_count(compiled.program);
    const int eventCount = onda_event_count(compiled.program);
    const int delegateCount = onda_delegate_count(compiled.program);
    const int logSiteCount = onda_log_site_count(compiled.program);
    const int outputCount = onda_output_count(compiled.program);
    const int bufferArrayCount = onda_buffer_array_count(compiled.program);

    if (inputCount < 0
        || paramCount < 0
        || eventCount < 0
        || delegateCount < 0
        || logSiteCount < 0
        || bufferCount < 0
        || outputCount < 0
        || bufferArrayCount < 0) {
        error = "Failed to query endpoint counts from Onda program.";
        return false;
    }

    compiled.hasDelegates = delegateCount > 0;
    compiled.hasPrints = logSiteCount > 0;

    std::vector<bool> bufferArraySlots(static_cast<size_t>(bufferCount), false);
    for (int i = 0; i < bufferArrayCount; ++i) {
        const int first = onda_buffer_array_first(compiled.program, i);
        const int length = onda_buffer_array_len(compiled.program, i);
        if (first < 0 || length < 1 || first > bufferCount - length) {
            error = "Failed to query buffer-array metadata from Onda program.";
            return false;
        }

        if (!compiledFromProject) {
            const char* name = onda_buffer_array_name(compiled.program, i);
            error = "Host constraint: buffer array '";
            error += name ? name : std::to_string(i);
            error += "' requires an .ondaproject default and cannot be supplied from SuperCollider.";
            return false;
        }

        for (int slot = first; slot < first + length; ++slot) {
            bufferArraySlots[static_cast<size_t>(slot)] = true;
        }
    }

    compiled.inputs.clear();
    compiled.outputs.clear();
    compiled.requiredInputChannels = 0;
    compiled.requiredOutputChannels = 0;

    const size_t totalInputDescriptors = static_cast<size_t>(inputCount)
        + static_cast<size_t>(paramCount)
        + static_cast<size_t>(eventCount)
        + static_cast<size_t>(bufferCount);
    compiled.inputs.reserve(totalInputDescriptors);
    compiled.outputs.reserve(static_cast<size_t>(outputCount));

    for (int i = 0; i < inputCount; ++i) {
        const int arrayLen = onda_input_array_len(compiled.program, i);
        if (arrayLen != 1) {
            std::ostringstream msg;
            msg << "Host constraint: input '";
            if (const char* name = onda_input_name(compiled.program, i)) {
                msg << name;
            } else {
                msg << i;
            }
            msg << "' must be a scalar for SuperCollider integration.";
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
        if (arrayLen != 1) {
            std::ostringstream msg;
            msg << "Host constraint: param '";
            if (const char* name = onda_param_name(compiled.program, i)) {
                msg << name;
            } else {
                msg << i;
            }
            msg << "' must be a scalar for SuperCollider integration.";
            error = msg.str();
            return false;
        }

        const int elemType = onda_param_elem_type(compiled.program, i);
        const int elemBytes = primitiveByteSize(elemType);
        const int typeBytes = onda_param_type_bytes(compiled.program, i);
        if (elemBytes < 0 || typeBytes != elemBytes) {
            std::ostringstream msg;
            msg << "Host constraint: param '";
            if (const char* name = onda_param_name(compiled.program, i)) {
                msg << name;
            } else {
                msg << i;
            }
            msg << "' must use a scalar primitive type for SuperCollider integration.";
            error = msg.str();
            return false;
        }

        OndaInputDescriptor desc;
        desc.name = onda_param_name(compiled.program, i) ? onda_param_name(compiled.program, i) : ("param" + std::to_string(i + 1));
        desc.audioRate = false;
        desc.kind = OndaInputKind::Param;
        desc.ondaIndex = i;
        desc.elemType = elemType;
        desc.elemBytes = elemBytes;
        desc.arrayLen = 1;

        if (onda_param_has_default(compiled.program, i) > 0) {
            desc.hasInit = true;
            desc.init = static_cast<float>(onda_param_default_f64(compiled.program, i));
        }

        compiled.inputs.push_back(std::move(desc));
    }

    for (int i = 0; i < eventCount; ++i) {
        const int eventParamCount = onda_event_param_count(compiled.program, i);
        if (eventParamCount != 1) {
            std::ostringstream msg;
            msg << "Host constraint: event '";
            if (const char* name = onda_event_name(compiled.program, i)) {
                msg << name;
            } else {
                msg << i;
            }
            msg << "' must use a single scalar primitive payload for SuperCollider integration.";
            error = msg.str();
            return false;
        }

        const int payloadBytes = onda_event_payload_bytes(compiled.program, i);
        const int elemType = onda_event_param_elem_type(compiled.program, i, 0);
        const int arrayLen = onda_event_param_array_len(compiled.program, i, 0);
        const int isArray = onda_event_param_is_array(compiled.program, i, 0);
        const int isSlice = onda_event_param_is_slice(compiled.program, i, 0);
        const int elemBytes = primitiveByteSize(elemType);

        if (elemBytes < 0
            || payloadBytes != elemBytes
            || arrayLen != 1
            || isArray != 0
            || isSlice != 0) {
            std::ostringstream msg;
            msg << "Host constraint: event '";
            if (const char* name = onda_event_name(compiled.program, i)) {
                msg << name;
            } else {
                msg << i;
            }
            msg << "' must use a single scalar primitive payload for SuperCollider integration.";
            error = msg.str();
            return false;
        }

        OndaInputDescriptor desc;
        desc.name = onda_event_name(compiled.program, i) ? onda_event_name(compiled.program, i) : ("event" + std::to_string(i + 1));
        desc.audioRate = false;
        desc.kind = OndaInputKind::Event;
        desc.ondaIndex = i;
        desc.elemType = elemType;
        desc.elemBytes = elemBytes;
        desc.arrayLen = 1;
        // Event controls start at zero and emit their declared scalar payload whenever changed.
        desc.hasInit = true;
        desc.init = 0.0f;

        compiled.inputs.push_back(std::move(desc));
    }

    for (int i = 0; i < bufferCount; ++i) {
        if (bufferArraySlots[static_cast<size_t>(i)]) {
            // SC has no buffer-array endpoint. Project compilation has already installed the
            // immutable asset (or Onda's neutral default) for every physical array slot.
            continue;
        }

        const char* rawName = onda_buffer_name(compiled.program, i);
        const std::string name = rawName ? rawName : ("buffer" + std::to_string(i + 1));
        const int elemType = onda_buffer_elem_type(compiled.program, i);
        if (elemType != ONDA_PRIMITIVE_F32) {
            if (compiledFromProject) {
                // The project-owned asset is already bound by Onda. Non-f32 buffers cannot be
                // overridden by an SC SndBuf, so they do not need an exposed UGen input. When the
                // manifest has no asset, the project image retains Onda's neutral default.
                continue;
            }
            std::ostringstream msg;
            msg << "Host constraint: buffer '";
            msg << name;
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
        desc.name = name;
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
        const int arrayLen = onda_output_array_len(compiled.program, i);
        if (arrayLen < 1) {
            error = "Failed to query output array length at index " + std::to_string(i) + ".";
            return false;
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
        if (arrayLen > std::numeric_limits<int>::max() / static_cast<int>(sizeof(float))) {
            error = "Output array byte layout exceeds the host limit at index " + std::to_string(i) + ".";
            return false;
        }
        const int expected = static_cast<int>(sizeof(float)) * arrayLen;
        if (typeBytes != expected) {
            std::ostringstream msg;
            msg << "Unexpected output byte layout at index " << i << ".";
            error = msg.str();
            return false;
        }

        OndaInputDescriptor outDesc;
        outDesc.name = onda_output_name(compiled.program, i) ? onda_output_name(compiled.program, i) : ("out" + std::to_string(i + 1));
        outDesc.ondaIndex = i;
        outDesc.elemType = ONDA_PRIMITIVE_F32;
        outDesc.elemBytes = static_cast<int>(sizeof(float));
        outDesc.arrayLen = arrayLen;

        if (arrayLen > std::numeric_limits<int>::max() - compiled.requiredOutputChannels) {
            error = "Flattened output count exceeds the host limit.";
            return false;
        }
        compiled.requiredOutputChannels += arrayLen;
        compiled.outputs.push_back(std::move(outDesc));
    }

    return true;
}

bool discoverProjectBufferDefaults(
    CompiledProgram& compiled,
    onda_instance_t* instance,
    std::string& error) {
    for (auto& desc : compiled.inputs) {
        if (desc.kind != OndaInputKind::Buffer) {
            continue;
        }

        const int result = onda_reset_buffer_to_project_default(instance, desc.ondaIndex);
        if (result == 0) {
            desc.hasProjectDefault = true;
            desc.init = -1.0f;
        } else if (result != -2) {
            error = "Failed to query project default for buffer '" + desc.name + "'.";
            return false;
        }
    }

    return true;
}

void writeReply(
    std::string& reply,
    int definitionId,
    int generation,
    const CompiledProgram& compiled) {
    reply = "_onda/";
    reply += std::to_string(definitionId);
    reply += "/";
    reply += std::to_string(generation);
    reply += "/";
    reply += std::to_string(compiled.inputs.size());

    for (const auto& input : compiled.inputs) {
        reply += "/";
        reply += input.name;
        reply += "/";
        reply += input.audioRate ? "0" : "1";
        reply += "/";
        reply += inputKindToTag(input.kind);
        reply += input.hasInit ? "/1/" : "/0/";
        reply += std::to_string(input.hasInit ? input.init : 0.0f);
    }

    reply += "/";
    reply += std::to_string(compiled.requiredOutputChannels);
}

struct OndaCompileCmdData {
    int definitionId = 0;
    int generation = 0;
    char* path = nullptr;
    char replyMsg[kMaxReplySize + 1] = {0};
    CompiledProgram* newProgram = nullptr;
    CompiledProgram* oldProgram = nullptr;
};

void writeFailureReply(OndaCompileCmdData& cmdData) {
    std::snprintf(
        cmdData.replyMsg,
        sizeof(cmdData.replyMsg),
        "_onda/%d/%d/_fail",
        cmdData.definitionId,
        cmdData.generation);
}

// RT cleanup stage (audio thread): release command payload allocated with RTAlloc.
void ondaCompileCleanup(World* world, void* inUserData) {
    auto* cmdData = static_cast<OndaCompileCmdData*>(inUserData);
    if (!cmdData) {
        return;
    }

    if (cmdData->path) {
        RTFree(world, cmdData->path);
    }

    cmdData->~OndaCompileCmdData();
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
    if (!cmdData) {
        return true;
    }

    if (!cmdData->newProgram) {
        Onda::observeGeneration(cmdData->definitionId, cmdData->generation);
        return true;
    }

    CompiledProgram* publishedProgram = cmdData->newProgram;
    const ProgramPublishResult publish = Onda::publishProgram(
        cmdData->definitionId,
        cmdData->generation,
        publishedProgram);

    if (publish.status == ProgramPublishStatus::Stale) {
        Print(
            "WARNING: Onda: discarded stale compile for definition %d generation %d.\n",
            cmdData->definitionId,
            cmdData->generation);
        writeFailureReply(*cmdData);
        return true;
    }
    if (publish.status == ProgramPublishStatus::InvalidDefinition) {
        Print(
            "ERROR: Onda: definition id %d is outside the supported range 1..%d.\n",
            cmdData->definitionId,
            static_cast<int>(Onda::patchStorage.size()));
        writeFailureReply(*cmdData);
        return true;
    }

    cmdData->oldProgram = publish.replacedProgram;

    if (publish.replacedProgram) {
        Onda* unit = publish.replacedProgram->firstUnit;
        while (unit) {
            Onda* next = unit->nextProgramUnit();
            unit->handleHotSwap(cmdData->definitionId, publishedProgram);
            unit = next;
        }
    }

    // Program is now owned by patch storage.
    cmdData->newProgram = nullptr;
    return true;
}

// NRT stage 2 (worker thread): compile source and build immutable host metadata.
bool ondaCompileStage2Impl(World* world, void* inUserData) {
    auto* cmdData = static_cast<OndaCompileCmdData*>(inUserData);
    if (!cmdData) {
        Print("ERROR: Onda: invalid compile command payload.\n");
        return false;
    }

    if (!cmdData->path) {
        Print("ERROR: Onda: compile command has no valid source path.\n");
        writeFailureReply(*cmdData);
        return true;
    }
    if (cmdData->definitionId < 1 || cmdData->definitionId > kMaxDefinitionId) {
        Print(
            "ERROR: Onda: definition id must be between 1 and %d (received %d).\n",
            kMaxDefinitionId,
            cmdData->definitionId);
        writeFailureReply(*cmdData);
        return true;
    }
    if (cmdData->generation < 1) {
        Print("ERROR: Onda: generation must be positive (received %d).\n", cmdData->generation);
        writeFailureReply(*cmdData);
        return true;
    }
    onda_compile_options_t compileOptions{};
    compileOptions.fast_math = 0;
    compileOptions.sample_rate = static_cast<float>(world->mSampleRate);
    compileOptions.block_size = world->mBufLength;

    ScopedOndaDiagnostic diag;
    std::unique_ptr<CompiledProgram, decltype(&destroyCompiledProgram)> compiled(
        new CompiledProgram(),
        destroyCompiledProgram);
    const bool compiledFromProject = isProjectPath(std::filesystem::u8path(cmdData->path));
    compiled->program = onda_compile_file(cmdData->path, &compileOptions, nullptr, diag.get());

    if (!compiled->program) {
        const char* message = diag->message ? diag->message : "unknown compile error";
        const char* diagnosticFile = diag->file ? diag->file : cmdData->path;
        Print(
            "ERROR: Onda: compile failed in '%s' (%d:%d): %s\n",
            diagnosticFile,
            diag->line,
            diag->column,
            message);
        writeFailureReply(*cmdData);
        return true;
    }

    std::string metaError;
    if (!buildProgramMetadata(*compiled, compiledFromProject, metaError)) {
        Print("ERROR: Onda: %s\n", metaError.c_str());
        writeFailureReply(*cmdData);
        return true;
    }

    if (compiledFromProject) {
        ScopedOndaDiagnostic instanceDiag;
        std::unique_ptr<onda_instance_t, decltype(&onda_instance_destroy)> instance(
            onda_instance_create(
                compiled->program,
                compiled->requiredInputChannels,
                (compiled->requiredOutputChannels > 0) ? compiled->requiredOutputChannels : 1,
                instanceDiag.get()),
            onda_instance_destroy);

        if (!instance) {
            const char* msg = instanceDiag->message ? instanceDiag->message : "unknown instance creation error";
            Print("ERROR: Onda: failed to inspect project defaults for definition %d: %s\n", cmdData->definitionId, msg);
            writeFailureReply(*cmdData);
            return true;
        }

        if (!discoverProjectBufferDefaults(*compiled, instance.get(), metaError)) {
            Print("ERROR: Onda: %s\n", metaError.c_str());
            writeFailureReply(*cmdData);
            return true;
        }
    }

    std::string reply;
    writeReply(reply, cmdData->definitionId, cmdData->generation, *compiled);

    if (reply.size() > static_cast<size_t>(kMaxReplySize)) {
        Print("ERROR: Onda: reply payload too large (%d).\n", static_cast<int>(reply.size()));
        writeFailureReply(*cmdData);
        return true;
    }

    std::strncpy(cmdData->replyMsg, reply.c_str(), sizeof(cmdData->replyMsg) - 1);
    cmdData->replyMsg[sizeof(cmdData->replyMsg) - 1] = '\0';
    cmdData->newProgram = compiled.release();

    Print(
        "Onda: compiled definition %d generation %d (%d inputs, %d flattened outputs).\n",
        cmdData->definitionId,
        cmdData->generation,
        static_cast<int>(cmdData->newProgram->inputs.size()),
        cmdData->newProgram->requiredOutputChannels);

    return true;
}

bool ondaCompileStage2(World* world, void* inUserData) {
    auto* cmdData = static_cast<OndaCompileCmdData*>(inUserData);
    try {
        return ondaCompileStage2Impl(world, inUserData);
    } catch (const std::exception& exception) {
        Print("ERROR: Onda: compile preparation failed: %s\n", exception.what());
    } catch (...) {
        Print("ERROR: Onda: compile preparation failed with an unknown exception.\n");
    }

    if (cmdData) {
        writeFailureReply(*cmdData);
    }
    return true;
}

void ondaCompile(World* inWorld, void* /*inUserData*/, struct sc_msg_iter* args, void* replyAddr) {
    auto* cmdData = static_cast<OndaCompileCmdData*>(RTAlloc(inWorld, sizeof(OndaCompileCmdData)));
    if (!cmdData) {
        Print("ERROR: Onda: failed to allocate compile command data.\n");
        return;
    }

    new (cmdData) OndaCompileCmdData{};

    cmdData->definitionId = args->geti();
    cmdData->generation = args->geti();
    const char* path = args->gets();

    if (path) {
        const size_t len = std::strlen(path);
        if (len <= kMaxPathBytes) {
            cmdData->path = static_cast<char*>(RTAlloc(inWorld, len + 1));
            if (cmdData->path) {
                std::memcpy(cmdData->path, path, len + 1);
            } else {
                Print("ERROR: Onda: failed to allocate compile path.\n");
                writeFailureReply(*cmdData);
            }
        } else {
            Print("ERROR: Onda: compile path exceeds the %d-byte limit.\n", static_cast<int>(kMaxPathBytes));
            writeFailureReply(*cmdData);
        }
    } else {
        writeFailureReply(*cmdData);
    }

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
    int definitionId = 0;
    int generation = 0;
    CompiledProgram* programToDelete = nullptr;
};

// RT cleanup stage (audio thread): release command payload allocated with RTAlloc.
void ondaFreeCleanup(World* world, void* inUserData) {
    auto* cmdData = static_cast<OndaFreeCmdData*>(inUserData);
    if (cmdData) {
        cmdData->~OndaFreeCmdData();
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

    cmdData->programToDelete = Onda::removeProgram(cmdData->definitionId, cmdData->generation);
    if (cmdData->programToDelete) {
        Onda* unit = cmdData->programToDelete->firstUnit;
        while (unit) {
            Onda* next = unit->nextProgramUnit();
            unit->handleFree(cmdData->definitionId);
            unit = next;
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

    new (cmdData) OndaFreeCmdData{};
    cmdData->definitionId = args->geti();
    cmdData->generation = args->geti();

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

CompiledProgram* Onda::getProgramById(int definitionId) {
    const PatchEntry* entry = patchEntryForDefinition(definitionId);
    return entry ? entry->program : nullptr;
}

void Onda::observeGeneration(int definitionId, int generation) {
    PatchEntry* entry = patchEntryForDefinition(definitionId);
    if (entry && generation > entry->generation) {
        entry->generation = generation;
    }
}

ProgramPublishResult Onda::publishProgram(int definitionId, int generation, CompiledProgram* program) {
    PatchEntry* entry = patchEntryForDefinition(definitionId);
    if (!entry || generation < 1 || !program) {
        return {ProgramPublishStatus::InvalidDefinition, nullptr};
    }
    if (generation <= entry->generation) {
        return {ProgramPublishStatus::Stale, nullptr};
    }

    CompiledProgram* replaced = entry->program;
    entry->generation = generation;
    entry->program = program;
    return {
        replaced ? ProgramPublishStatus::Replaced : ProgramPublishStatus::Inserted,
        replaced,
    };
}

CompiledProgram* Onda::removeProgram(int definitionId, int generation) {
    PatchEntry* entry = patchEntryForDefinition(definitionId);
    if (!entry || generation < 1 || generation <= entry->generation) {
        return nullptr;
    }

    entry->generation = generation;
    CompiledProgram* old = entry->program;
    entry->program = nullptr;
    return old;
}

bool Onda::bindProgram(CompiledProgram* program, bool isHotSwap) {
    if (!program || !program->program) {
        return false;
    }

    const onda_allocator_t allocator{mWorld, ondaRtAlloc, ondaRtFree};
    onda_instance_t* newInstance = onda_instance_create_with_allocator(
        program->program,
        program->requiredInputChannels,
        (program->requiredOutputChannels > 0) ? program->requiredOutputChannels : 1,
        &allocator,
        nullptr);
    if (!newInstance) {
        Print(
            "ERROR: Onda (definition %d, instance %d): failed to allocate runtime instance. "
            "Increase the server's real-time memory pool.\n",
            mDefinitionId,
            mUnitIndex);
        return false;
    }

    if (!allocateRtState(program)) {
        onda_instance_destroy(newInstance);
        return false;
    }

    releaseInstance();

    mInstance = newInstance;
    attachToProgram(program);

    if (!initializeInstance()) {
        releaseInstance();
        freeRtState();
        return false;
    }

    mCalcFunc = make_calc_function<Onda, &Onda::next>();

    if (isHotSwap) {
        Print("Onda (definition %d, instance %d): hot-swapped.\n", mDefinitionId, mUnitIndex);
    }

    return true;
}

bool Onda::bindLatestProgram(bool isHotSwap) {
    CompiledProgram* latest = getProgramById(mDefinitionId);
    if (!latest) {
        return false;
    }

    if (latest == mProgram && mInstance) {
        return true;
    }

    return bindProgram(latest, isHotSwap);
}

bool Onda::initializeInstance() {
    const BufferPrepResult bufferPrep = prepareBuffers();
    if (bufferPrep == BufferPrepResult::Fatal) {
#if SUPERNOVA
        releaseBufferLocks();
#endif
        return false;
    }

    if (!prepareInputs() || !prepareParams()) {
#if SUPERNOVA
        releaseBufferLocks();
#endif
        return false;
    }

    const int result = onda_init(mInstance, ONDA_INIT_FULL, executionOutput());
    flushExecutionOutput();
#if SUPERNOVA
    releaseBufferLocks();
#endif
    if (result != 0) {
        Print(
            "ERROR: Onda (definition %d, instance %d): full initialization failed.\n",
            mDefinitionId,
            mUnitIndex);
        return false;
    }

    return true;
}

void Onda::attachToProgram(CompiledProgram* program) {
    mProgram = program;
    mPreviousProgramUnit = nullptr;
    mNextProgramUnit = program->firstUnit;
    if (mNextProgramUnit) {
        mNextProgramUnit->mPreviousProgramUnit = this;
    }
    program->firstUnit = this;
}

void Onda::detachFromProgram() {
    if (!mProgram) {
        mPreviousProgramUnit = nullptr;
        mNextProgramUnit = nullptr;
        return;
    }

    if (mPreviousProgramUnit) {
        mPreviousProgramUnit->mNextProgramUnit = mNextProgramUnit;
    } else if (mProgram->firstUnit == this) {
        mProgram->firstUnit = mNextProgramUnit;
    }
    if (mNextProgramUnit) {
        mNextProgramUnit->mPreviousProgramUnit = mPreviousProgramUnit;
    }

    mProgram = nullptr;
    mPreviousProgramUnit = nullptr;
    mNextProgramUnit = nullptr;
}

bool Onda::allocateRtState(CompiledProgram* program) {
    const OndaInputDescriptor* newBoundInputs = program->inputs.data();
    const int newBoundInputCount = static_cast<int>(program->inputs.size());
    const OndaInputDescriptor* newBoundOutputs = program->outputs.data();
    const int newBoundOutputCount = static_cast<int>(program->outputs.size());
    int newAudioInputDescCount = 0;
    int newParamDescCount = 0;
    int newEventDescCount = 0;
    int newBufferDescCount = 0;

    const int scInputCount = numInputs() - 1;
    if (newBoundInputCount != scInputCount) {
        Print(
            "ERROR: Onda (definition %d, instance %d): program requires exactly %d SC inputs but UGen has %d. "
            "Relaunch the UGen with the new IO shape.\n",
            mDefinitionId,
            mUnitIndex,
            newBoundInputCount,
            scInputCount);
        return false;
    }

    if (program->requiredOutputChannels != numOutputs()) {
        Print(
            "ERROR: Onda (definition %d, instance %d): program requires exactly %d output channels but UGen has %d. "
            "Relaunch the UGen with the new IO shape.\n",
            mDefinitionId,
            mUnitIndex,
            program->requiredOutputChannels,
            numOutputs());
        return false;
    }

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

    int usedScOuts = 0;
    size_t totalScratchBytes = 0;
    for (int i = 0; i < newBoundOutputCount; ++i) {
        const auto& outDesc = newBoundOutputs[i];
        usedScOuts += outDesc.arrayLen;
        if (outDesc.arrayLen == 1) {
            continue;
        }

        const size_t scratchBytes = sizeof(float)
            * static_cast<size_t>(outDesc.arrayLen)
            * static_cast<size_t>(bufferSize());
        if (scratchBytes > static_cast<size_t>(std::numeric_limits<int>::max())
            || scratchBytes > std::numeric_limits<size_t>::max() - totalScratchBytes) {
            Print(
                "ERROR: Onda (definition %d, instance %d): output scratch layout exceeds the host limit.\n",
                mDefinitionId,
                mUnitIndex);
            return false;
        }
        totalScratchBytes += scratchBytes;
    }
    if (usedScOuts != program->requiredOutputChannels) {
        Print(
            "ERROR: Onda (definition %d, instance %d): internal output mapping mismatch (%d mapped, %d required).\n",
            mDefinitionId,
            mUnitIndex,
            usedScOuts,
            program->requiredOutputChannels);
        return false;
    }

    constexpr size_t noOffset = std::numeric_limits<size_t>::max();
    size_t storageBytes = 0;
    bool layoutOverflow = false;
    auto reserve = [&](size_t bytes, size_t alignment) {
        if (bytes == 0) {
            return noOffset;
        }
        const size_t padding = (alignment - (storageBytes % alignment)) % alignment;
        if (padding > std::numeric_limits<size_t>::max() - storageBytes
            || bytes > std::numeric_limits<size_t>::max() - storageBytes - padding) {
            layoutOverflow = true;
            return noOffset;
        }
        storageBytes += padding;
        const size_t offset = storageBytes;
        storageBytes += bytes;
        return offset;
    };
    auto reserveArray = [&](int count, size_t elementBytes, size_t alignment) {
        if (count <= 0) {
            return noOffset;
        }
        const size_t unsignedCount = static_cast<size_t>(count);
        if (unsignedCount > std::numeric_limits<size_t>::max() / elementBytes) {
            layoutOverflow = true;
            return noOffset;
        }
        return reserve(unsignedCount * elementBytes, alignment);
    };

    const size_t audioOffset = reserveArray(newAudioInputDescCount, sizeof(RuntimeAudioInputState), alignof(RuntimeAudioInputState));
    const size_t paramOffset = reserveArray(newParamDescCount, sizeof(RuntimeControlState), alignof(RuntimeControlState));
    const size_t eventOffset = reserveArray(newEventDescCount, sizeof(RuntimeControlState), alignof(RuntimeControlState));
    const size_t bufferOffset = reserveArray(newBufferDescCount, sizeof(RuntimeBufferState), alignof(RuntimeBufferState));
    const size_t outputOffset = reserveArray(newBoundOutputCount, sizeof(RuntimeOutputState), alignof(RuntimeOutputState));
#if SUPERNOVA
    const size_t lockOffset = reserveArray(newBufferDescCount, sizeof(BufferLockState), alignof(BufferLockState));
#endif
    const size_t scratchOffset = reserve(totalScratchBytes, alignof(float));
    const size_t delegateBatchOffset = reserve(program->hasDelegates ? kDelegateBatchBytes : 0, alignof(uint8_t));
    const size_t printBatchOffset = reserve(program->hasPrints ? kPrintBatchBytes : 0, alignof(uint8_t));
    const size_t formatOffset = reserve(
        program->hasDelegates ? kDelegateFormatBytes : (program->hasPrints ? kPrintFormatBytes : 0),
        alignof(char));

    if (layoutOverflow) {
        Print("ERROR: Onda (definition %d, instance %d): RT state layout exceeds the host limit.\n", mDefinitionId, mUnitIndex);
        return false;
    }

    auto* newStorage = storageBytes > 0
        ? static_cast<uint8_t*>(RTAlloc(mWorld, storageBytes))
        : nullptr;
    if (storageBytes > 0 && !newStorage) {
        Print(
            "ERROR: Onda (definition %d, instance %d): failed to allocate %zu bytes of RT host state.\n",
            mDefinitionId,
            mUnitIndex,
            storageBytes);
        return false;
    }
    if (newStorage) {
        std::memset(newStorage, 0, storageBytes);
    }
    auto at = [&](size_t offset) -> uint8_t* {
        return offset == noOffset ? nullptr : newStorage + offset;
    };

    auto* newRuntimeAudioInputs = reinterpret_cast<RuntimeAudioInputState*>(at(audioOffset));
    auto* newRuntimeParams = reinterpret_cast<RuntimeControlState*>(at(paramOffset));
    auto* newRuntimeEvents = reinterpret_cast<RuntimeControlState*>(at(eventOffset));
    auto* newRuntimeBuffers = reinterpret_cast<RuntimeBufferState*>(at(bufferOffset));
    auto* newRuntimeOutputs = reinterpret_cast<RuntimeOutputState*>(at(outputOffset));
    auto* newOutputScratch = at(scratchOffset);

    int audioCursor = 0;
    int paramCursor = 0;
    int eventCursor = 0;
    int bufferCursor = 0;
    for (int i = 0; i < newBoundInputCount; ++i) {
        switch (newBoundInputs[i].kind) {
            case OndaInputKind::Input:
                new (newRuntimeAudioInputs + audioCursor) RuntimeAudioInputState{};
                newRuntimeAudioInputs[audioCursor++].descriptorIndex = i;
                break;
            case OndaInputKind::Param:
                new (newRuntimeParams + paramCursor) RuntimeControlState{};
                newRuntimeParams[paramCursor++].descriptorIndex = i;
                break;
            case OndaInputKind::Event:
                new (newRuntimeEvents + eventCursor) RuntimeControlState{};
                newRuntimeEvents[eventCursor++].descriptorIndex = i;
                break;
            case OndaInputKind::Buffer:
                new (newRuntimeBuffers + bufferCursor) RuntimeBufferState{};
                newRuntimeBuffers[bufferCursor++].descriptorIndex = i;
                break;
        }
    }

    uint8_t* scratchCursor = newOutputScratch;
    usedScOuts = 0;
    for (int i = 0; i < newBoundOutputCount; ++i) {
        new (newRuntimeOutputs + i) RuntimeOutputState{};
        auto& state = newRuntimeOutputs[i];
        const auto& outDesc = newBoundOutputs[i];
        state.scOffset = usedScOuts;
        state.directBind = outDesc.arrayLen == 1;
        usedScOuts += outDesc.arrayLen;
        if (!state.directBind) {
            state.scratchBytes = static_cast<int>(
                sizeof(float) * static_cast<size_t>(outDesc.arrayLen) * static_cast<size_t>(bufferSize()));
            state.scratch = scratchCursor;
            scratchCursor += state.scratchBytes;
        }
    }

    freeRtState();

    mRtStorage = newStorage;
    mRuntimeAudioInputs = newRuntimeAudioInputs;
    mAudioInputDescCount = newAudioInputDescCount;
    mRuntimeParams = newRuntimeParams;
    mParamDescCount = newParamDescCount;
    mRuntimeEvents = newRuntimeEvents;
    mEventDescCount = newEventDescCount;
    mRuntimeBuffers = newRuntimeBuffers;
    mBufferDescCount = newBufferDescCount;
    mRuntimeOutputs = newRuntimeOutputs;
    mDelegateBatchStorage = at(delegateBatchOffset);
    mPrintBatchStorage = at(printBatchOffset);
    mDelegateFormatStorage = program->hasDelegates ? reinterpret_cast<char*>(at(formatOffset)) : nullptr;
    mPrintFormatStorage = program->hasPrints ? reinterpret_cast<char*>(at(formatOffset)) : nullptr;
    mDelegateBatch = {};
    mDelegateBatch.storage = mDelegateBatchStorage;
    mDelegateBatch.capacity_bytes = mDelegateBatchStorage ? kDelegateBatchBytes : 0;
    mPrintBatch = {};
    mPrintBatch.storage = mPrintBatchStorage;
    mPrintBatch.capacity_bytes = mPrintBatchStorage ? kPrintBatchBytes : 0;
    mExecutionOutput = {};
    mExecutionOutput.delegate_batch = mDelegateBatchStorage ? &mDelegateBatch : nullptr;
    mExecutionOutput.print_batch = mPrintBatchStorage ? &mPrintBatch : nullptr;
    mNeedsOutputCopy = totalScratchBytes > 0;
#if SUPERNOVA
    mBufferLocks = reinterpret_cast<BufferLockState*>(at(lockOffset));
    for (int i = 0; i < newBufferDescCount; ++i) {
        new (mBufferLocks + i) BufferLockState{};
    }
    mBufferLockCapacity = newBufferDescCount;
    mBufferLockCount = 0;
#endif
    mBindingsNeedValidate = false;

    return true;
}

void Onda::freeRtState() {
    if (mRtStorage) {
        RTFree(mWorld, mRtStorage);
        mRtStorage = nullptr;
    }

    mRuntimeAudioInputs = nullptr;
    mRuntimeParams = nullptr;
    mRuntimeEvents = nullptr;
    mRuntimeBuffers = nullptr;
    mRuntimeOutputs = nullptr;
    mDelegateBatchStorage = nullptr;
    mPrintBatchStorage = nullptr;
    mDelegateFormatStorage = nullptr;
    mPrintFormatStorage = nullptr;
    mDelegateBatch = {};
    mPrintBatch = {};
    mExecutionOutput = {};
    mNeedsOutputCopy = false;

#if SUPERNOVA
    mBufferLocksHeld = false;
    mBufferLocks = nullptr;
#endif

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
    onda_instance_t* instance = mInstance;
    mInstance = nullptr;
    detachFromProgram();

    if (!instance) {
        return;
    }

    onda_instance_destroy(instance);
}

void Onda::handleHotSwap(int definitionId, CompiledProgram* program) {
    if (definitionId != mDefinitionId || !program) {
        return;
    }

    if (program == mProgram && mInstance) {
        return;
    }

    if (!bindProgram(program, true)) {
        Print(
            "WARNING: Onda (definition %d, instance %d): hot-swap failed. Releasing current instance and silencing unit.\n",
            mDefinitionId,
            mUnitIndex);
        releaseInstance();
        freeRtState();
        setSilence();
    }
}

void Onda::handleFree(int definitionId) {
    if (definitionId != mDefinitionId) {
        return;
    }

    releaseInstance();
    freeRtState();
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

    if (!std::isfinite(buf->samplerate) || buf->samplerate <= 0.0) {
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

onda_execution_output_t* Onda::executionOutput() {
    return (mExecutionOutput.delegate_batch || mExecutionOutput.print_batch)
        ? &mExecutionOutput
        : nullptr;
}

void Onda::emitDelegateOccurrence(const onda_delegate_occurrence_t& occurrence) const {
    if (!mProgram || !mProgram->program) {
        return;
    }

    const int delegateCount = onda_delegate_count(mProgram->program);
    if (occurrence.delegate_index >= static_cast<uint32_t>(std::max(delegateCount, 0))) {
        Print(
            "WARNING: Onda (definition %d, instance %d): ignored delegate occurrence with invalid index %u.\n",
            mDefinitionId,
            mUnitIndex,
            occurrence.delegate_index);
        return;
    }

    const int delegateIndex = static_cast<int>(occurrence.delegate_index);
    const char* delegateName = onda_delegate_name(mProgram->program, delegateIndex);
    const int paramCount = onda_delegate_param_count(mProgram->program, delegateIndex);
    if (paramCount < 0) {
        Print(
            "WARNING: Onda (definition %d, instance %d): failed to inspect delegate %u.\n",
            mDefinitionId,
            mUnitIndex,
            occurrence.delegate_index);
        return;
    }

    if (!mDelegateFormatStorage) {
        Print(
            "WARNING: Onda (definition %d, instance %d): no delegate formatting storage is available.\n",
            mDefinitionId,
            mUnitIndex);
        return;
    }

    const uint8_t* cursor = occurrence.payload;
    const uint8_t* end = cursor ? cursor + occurrence.payload_size_bytes : cursor;
    size_t length = 0;
    bool formatValid = appendFormatted(
        mDelegateFormatStorage,
        kDelegateFormatBytes,
        length,
        "Onda: delegate %s(",
        delegateName ? delegateName : "<unnamed>");

    bool payloadValid = true;
    for (int paramIndex = 0; paramIndex < paramCount && payloadValid && formatValid; ++paramIndex) {
        if (paramIndex > 0) {
            formatValid = appendFormatted(mDelegateFormatStorage, kDelegateFormatBytes, length, ", ");
        }

        const char* paramName = onda_delegate_param_name(mProgram->program, delegateIndex, paramIndex);
        if (formatValid) {
            formatValid = appendFormatted(
                mDelegateFormatStorage,
                kDelegateFormatBytes,
                length,
                "%s: ",
                paramName ? paramName : "<unnamed>");
        }

        const int elemType = onda_delegate_param_elem_type(mProgram->program, delegateIndex, paramIndex);
        const int elemBytes = primitiveByteSize(elemType);
        const int isSlice = onda_delegate_param_is_slice(mProgram->program, delegateIndex, paramIndex);
        const int isArray = onda_delegate_param_is_array(mProgram->program, delegateIndex, paramIndex);
        const int arrayLen = onda_delegate_param_array_len(mProgram->program, delegateIndex, paramIndex);
        if (elemBytes < 0 || isSlice < 0 || isArray < 0 || arrayLen < 0) {
            payloadValid = false;
            break;
        }

        uint32_t count = 1;
        if (isSlice > 0) {
            payloadValid = readPackedValue(cursor, end, count);
        } else if (isArray > 0) {
            count = static_cast<uint32_t>(arrayLen);
        }

        if (!payloadValid
            || static_cast<size_t>(count) > std::numeric_limits<size_t>::max() / static_cast<size_t>(elemBytes)
            || !cursor
            || !end
            || cursor > end
            || static_cast<size_t>(end - cursor) < static_cast<size_t>(count) * static_cast<size_t>(elemBytes)) {
            payloadValid = false;
            break;
        }

        const bool collection = isSlice > 0 || isArray > 0;
        if (collection) {
            formatValid = appendFormatted(mDelegateFormatStorage, kDelegateFormatBytes, length, "[");
        }
        for (uint32_t valueIndex = 0; valueIndex < count && formatValid; ++valueIndex) {
            if (valueIndex > 0) {
                formatValid = appendFormatted(mDelegateFormatStorage, kDelegateFormatBytes, length, ", ");
            }
            if (formatValid) {
                const bool appended = appendPackedPrimitive(
                    elemType,
                    cursor,
                    end,
                    mDelegateFormatStorage,
                    kDelegateFormatBytes,
                    length,
                    payloadValid);
                if (!appended) {
                    // Payload failures are reported in the line below; formatting failures use
                    // the capacity warning after the loop.
                    if (payloadValid) {
                        formatValid = false;
                    }
                    break;
                }
            }
        }
        if (collection && formatValid) {
            formatValid = appendFormatted(mDelegateFormatStorage, kDelegateFormatBytes, length, "]");
        }
    }

    if (formatValid && (!payloadValid || cursor != end)) {
        formatValid = appendFormatted(mDelegateFormatStorage, kDelegateFormatBytes, length, "<invalid payload>");
    }
    if (formatValid) {
        formatValid = appendFormatted(mDelegateFormatStorage, kDelegateFormatBytes, length, ")\n");
    }

    if (formatValid) {
        Print("%s", mDelegateFormatStorage);
    } else {
        Print(
            "WARNING: Onda (definition %d, instance %d): delegate occurrence exceeded host formatting capacity.\n",
            mDefinitionId,
            mUnitIndex);
    }
}

void Onda::emitPrintOccurrence(const onda_print_occurrence_t& occurrence) const {
    const uint64_t recordBytes = static_cast<uint64_t>(ONDA_PRINT_RECORD_HEADER_SIZE)
        + static_cast<uint64_t>(occurrence.payload_size_bytes);
    if (!mInstance
        || !mPrintFormatStorage
        || !occurrence.payload
        || recordBytes > std::numeric_limits<uint32_t>::max()) {
        Print(
            "WARNING: Onda (definition %d, instance %d): ignored an invalid print occurrence.\n",
            mDefinitionId,
            mUnitIndex);
        return;
    }

    onda_print_batch_t single{};
    single.storage = const_cast<uint8_t*>(occurrence.payload) - ONDA_PRINT_RECORD_HEADER_SIZE;
    single.capacity_bytes = static_cast<uint32_t>(recordBytes);
    single.used_bytes = static_cast<uint32_t>(recordBytes);
    single.record_count = 1;

    size_t required = 0;
    const int status = onda_format_print_batch_into(
        mInstance,
        &single,
        mPrintFormatStorage,
        kPrintFormatBytes,
        &required,
        nullptr);
    if (status != 0) {
        Print(
            "WARNING: Onda (definition %d, instance %d): could not format print occurrence.\n",
            mDefinitionId,
            mUnitIndex);
        return;
    }

    // Onda deliberately returns success for a size query or undersized destination and leaves the
    // destination untouched. Print only when the complete text and its NUL terminator fit.
    if (required >= kPrintFormatBytes) {
        Print(
            "WARNING: Onda (definition %d, instance %d): print occurrence exceeds the %zu-byte "
            "host formatting capacity (%zu UTF-8 bytes required).\n",
            mDefinitionId,
            mUnitIndex,
            kPrintFormatBytes,
            required);
        return;
    }

    Print("%s", mPrintFormatStorage);
}

void Onda::flushExecutionOutput() {
    onda_batch_cursor_t delegateCursor{};
    onda_batch_cursor_t printCursor{};
    onda_delegate_occurrence_t delegateOccurrence{};
    onda_print_occurrence_t printOccurrence{};
    bool hasDelegate = mExecutionOutput.delegate_batch
        && onda_delegate_batch_next(&mDelegateBatch, &delegateCursor, &delegateOccurrence) > 0;
    bool hasPrint = mExecutionOutput.print_batch
        && onda_print_batch_next(&mPrintBatch, &printCursor, &printOccurrence) > 0;

    while (hasDelegate || hasPrint) {
        const bool emitPrint = hasPrint
            && (!hasDelegate || printOccurrence.sequence <= delegateOccurrence.sequence);
        if (emitPrint) {
            emitPrintOccurrence(printOccurrence);
            hasPrint = onda_print_batch_next(&mPrintBatch, &printCursor, &printOccurrence) > 0;
        } else {
            emitDelegateOccurrence(delegateOccurrence);
            hasDelegate = onda_delegate_batch_next(&mDelegateBatch, &delegateCursor, &delegateOccurrence) > 0;
        }
    }

    if (mDelegateBatch.overflow_count > 0) {
        Print(
            "WARNING: Onda (definition %d, instance %d): %u delegate occurrence(s) exceeded host output capacity.\n",
            mDefinitionId,
            mUnitIndex,
            mDelegateBatch.overflow_count);
    }
    if (mPrintBatch.overflow_count > 0) {
        Print(
            "WARNING: Onda (definition %d, instance %d): %u print occurrence(s) exceeded host output capacity.\n",
            mDefinitionId,
            mUnitIndex,
            mPrintBatch.overflow_count);
    }

    if (mExecutionOutput.delegate_batch) {
        onda_delegate_batch_reset(&mDelegateBatch);
    }
    if (mExecutionOutput.print_batch) {
        onda_print_batch_reset(&mPrintBatch);
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
        const int i = mRuntimeBuffers[n].descriptorIndex;
        const auto& desc = mProgram->inputs[i];

        const int scSlot = i + 1;
        const float fallback = desc.hasInit ? desc.init : 0.0f;
        const float rawBufNum = (scSlot > 0) ? in0(scSlot) : fallback;
        const int bufIndex = scBufferIndex(rawBufNum);
        SndBuf* buf = resolveSndBufByIndex(bufIndex);

        if (isValidBufferBinding(buf, desc) && !addOrUpgradeBufferLock(buf, desc.bufferMayWrite)) {
            Print("ERROR: Onda (definition %d, instance %d): buffer lock state capacity exceeded.\n", mDefinitionId, mUnitIndex);
            return BufferPrepResult::Fatal;
        }
    }

    acquireBufferLocks();
#endif

    bool allValid = true;

    for (int n = 0; n < mBufferDescCount; ++n) {
        auto& state = mRuntimeBuffers[n];
        const int i = state.descriptorIndex;
        const auto& desc = mProgram->inputs[i];

        const int scSlot = i + 1;
        const float fallback = desc.hasInit ? desc.init : 0.0f;
        const float rawBufNum = (scSlot > 0) ? in0(scSlot) : fallback;
        const int bufIndex = scBufferIndex(rawBufNum);
        SndBuf* buf = resolveSndBufByIndex(bufIndex);
        const bool valid = isValidBufferBinding(buf, desc);
        const bool useProjectDefault = !valid && desc.hasProjectDefault;
        allValid = allValid && (valid || useProjectDefault);

        if (useProjectDefault) {
            if (state.bound) {
                if (onda_reset_buffer_to_project_default(mInstance, desc.ondaIndex) != 0) {
                    Print(
                        "ERROR: Onda (definition %d, instance %d): failed to restore project buffer '%s'.\n",
                        mDefinitionId,
                        mUnitIndex,
                        desc.name.c_str());
                    return BufferPrepResult::Fatal;
                }
                state.bound = false;
                state.boundPtr = nullptr;
                state.boundBufIndex = -1;
                state.boundFrames = -1;
                state.boundChannels = -1;
                state.boundSampleRate = 0.0f;
                mBindingsNeedValidate = true;
            }
            state.invalidReported = false;
            continue;
        }

        if (!valid && !state.invalidReported) {
            Print(
                "WARNING: Onda (definition %d, instance %d): buffer '%s' is unavailable or incompatible (bufnum=%d); "
                "outputting silence until valid.\n",
                mDefinitionId,
                mUnitIndex,
                desc.name.c_str(),
                bufIndex);
            state.invalidReported = true;
        } else if (valid) {
            state.invalidReported = false;
        }

        if (!valid) {
            if (state.bound) {
                if (onda_bind_buffer(mInstance, desc.ondaIndex, nullptr, 0, 0, 0.0f, ONDA_PRIMITIVE_F32) != 0) {
                    Print(
                        "ERROR: Onda (definition %d, instance %d): failed to unbind buffer '%s' (bufnum=%d).\n",
                        mDefinitionId,
                        mUnitIndex,
                        desc.name.c_str(),
                        bufIndex);
                    return BufferPrepResult::Fatal;
                }
                state.bound = false;
                state.boundPtr = nullptr;
                state.boundBufIndex = -1;
                state.boundFrames = -1;
                state.boundChannels = -1;
                state.boundSampleRate = 0.0f;
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
            || state.boundPtr != ptr
            || state.boundFrames != frames
            || state.boundChannels != channels
            || state.boundSampleRate != sampleRate;

        if (needsRebind) {
            if (onda_bind_buffer(mInstance, desc.ondaIndex, ptr, frames, channels, sampleRate, ONDA_PRIMITIVE_F32) != 0) {
                Print(
                    "ERROR: Onda (definition %d, instance %d): failed to bind buffer '%s' (bufnum=%d).\n",
                    mDefinitionId,
                    mUnitIndex,
                    desc.name.c_str(),
                    bufIndex);
                return BufferPrepResult::Fatal;
            }
            state.bound = true;
            state.boundPtr = ptr;
            state.boundBufIndex = bufIndex;
            state.boundFrames = frames;
            state.boundChannels = channels;
            state.boundSampleRate = sampleRate;
            mBindingsNeedValidate = true;
        }
    }

    return allValid ? BufferPrepResult::Ok : BufferPrepResult::Invalid;
}

bool Onda::prepareInputs() {
    for (int n = 0; n < mAudioInputDescCount; ++n) {
        auto& state = mRuntimeAudioInputs[n];
        const int i = state.descriptorIndex;
        const auto& desc = mProgram->inputs[i];

        const int scSlot = i + 1;
        const float* src = (scSlot > 0) ? in(scSlot) : nullptr;

        if (!src) {
            Print(
                "ERROR: Onda (definition %d, instance %d): missing bound audio input '%s'. Relaunch the UGen with the new IO shape.\n",
                mDefinitionId,
                mUnitIndex,
                desc.name.c_str());
            return false;
        }

        const void* bindPtr = src;
        const int bytes = static_cast<int>(sizeof(float) * static_cast<size_t>(bufferSize()));
        const bool needsRebind = !state.bound || state.boundPtr != bindPtr || state.boundBytes != bytes;
        if (needsRebind) {
            if (onda_bind_input(mInstance, desc.ondaIndex, bindPtr, bytes) != 0) {
                Print("ERROR: Onda (definition %d, instance %d): failed to bind input '%s'.\n", mDefinitionId, mUnitIndex, desc.name.c_str());
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

bool Onda::prepareParams() {
    for (int n = 0; n < mParamDescCount; ++n) {
        auto& state = mRuntimeParams[n];
        const int i = state.descriptorIndex;
        const auto& desc = mProgram->inputs[i];
        const int scSlot = i + 1;
        const float fallback = desc.hasInit ? desc.init : 0.0f;
        const float value = (scSlot > 0) ? in0(scSlot) : fallback;

        if (!state.initialized || state.previousValue != value) {
            std::array<uint8_t, sizeof(double)> payload{};
            if (!packControlPrimitive(value, desc.elemType, payload)) {
                Print(
                    "ERROR: Onda (definition %d, instance %d): param '%s' has an unsupported type.\n",
                    mDefinitionId,
                    mUnitIndex,
                    desc.name.c_str());
                return false;
            }

            if (onda_set_param_by_index(mInstance, desc.ondaIndex, payload.data(), desc.elemBytes) != 0) {
                Print("ERROR: Onda (definition %d, instance %d): failed to set param '%s'.\n", mDefinitionId, mUnitIndex, desc.name.c_str());
                return false;
            }
            state.previousValue = value;
            state.initialized = true;
        }
    }

    return true;
}

bool Onda::triggerEvents() {
    for (int n = 0; n < mEventDescCount; ++n) {
        auto& state = mRuntimeEvents[n];
        const int i = state.descriptorIndex;
        const auto& desc = mProgram->inputs[i];
        const int scSlot = i + 1;
        const float value = (scSlot > 0) ? in0(scSlot) : 0.0f;
        const bool triggered = state.previousValue != value;
        state.previousValue = value;

        if (triggered) {
            std::array<uint8_t, sizeof(double)> payload{};
            if (!packControlPrimitive(value, desc.elemType, payload)) {
                Print(
                    "ERROR: Onda (definition %d, instance %d): event '%s' has an unsupported payload type.\n",
                    mDefinitionId,
                    mUnitIndex,
                    desc.name.c_str());
                return false;
            }

            const int result = onda_trigger_event_by_index(
                mInstance,
                desc.ondaIndex,
                payload.data(),
                desc.elemBytes,
                executionOutput());
            flushExecutionOutput();
            if (result != 0) {
                Print("ERROR: Onda (definition %d, instance %d): failed to trigger event '%s'.\n", mDefinitionId, mUnitIndex, desc.name.c_str());
                return false;
            }
        }
    }

    return true;
}

bool Onda::prepareOutputs() {
    for (int i = 0; i < static_cast<int>(mProgram->outputs.size()); ++i) {
        const auto& outDesc = mProgram->outputs[i];
        auto& outState = mRuntimeOutputs[i];

        void* ptr = nullptr;
        int bytes = outDesc.elemBytes * outDesc.arrayLen * bufferSize();

        if (outState.directBind) {
            ptr = out(outState.scOffset);
            bytes = static_cast<int>(sizeof(float) * static_cast<size_t>(bufferSize()));
        } else {
            ptr = outState.scratch;
        }

        const bool needsRebind = !outState.bound || outState.boundPtr != ptr || outState.boundBytes != bytes;
        if (needsRebind) {
            if (onda_bind_output(mInstance, outDesc.ondaIndex, ptr, bytes) != 0) {
                Print("ERROR: Onda (definition %d, instance %d): failed to bind output '%s'.\n", mDefinitionId, mUnitIndex, outDesc.name.c_str());
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
    for (int i = 0; i < static_cast<int>(mProgram->outputs.size()); ++i) {
        const auto& outDesc = mProgram->outputs[i];
        auto& outState = mRuntimeOutputs[i];

        if (outState.directBind) {
            continue;
        }

        const uint8_t* base = outState.scratch;
        for (int c = 0; c < outDesc.arrayLen; ++c) {
            float* dst = out(outState.scOffset + c);
            const uint8_t* srcChan = base + (static_cast<size_t>(c) * static_cast<size_t>(bufferSize()) * static_cast<size_t>(outDesc.elemBytes));
            std::memcpy(dst, srcChan, sizeof(float) * static_cast<size_t>(nSamples));
        }
    }
}

void Onda::silenceBlockOutputs(int nSamples) {
    for (int i = 0; i < numOutputs(); ++i) {
        std::memset(out(i), 0, sizeof(float) * static_cast<size_t>(nSamples));
    }
}

bool Onda::processAudio(int nSamples) {
    if (nSamples != bufferSize()) {
        Print(
            "ERROR: Onda (definition %d, instance %d): invalid process size %d (compiled block size is %d).\n",
            mDefinitionId,
            mUnitIndex,
            nSamples,
            bufferSize());
        return false;
    }

    const BufferPrepResult bufferPrep = prepareBuffers();
    if (bufferPrep == BufferPrepResult::Fatal) {
#if SUPERNOVA
        releaseBufferLocks();
#endif
        return false;
    }

    if (bufferPrep == BufferPrepResult::Invalid) {
#if SUPERNOVA
        releaseBufferLocks();
#endif
        silenceBlockOutputs(nSamples);
        return true;
    }

    if (!prepareInputs()) {
#if SUPERNOVA
        releaseBufferLocks();
#endif
        return false;
    }

    if (!prepareParams()) {
#if SUPERNOVA
        releaseBufferLocks();
#endif
        return false;
    }

    if (!triggerEvents()) {
#if SUPERNOVA
        releaseBufferLocks();
#endif
        return false;
    }

    if (!prepareOutputs()) {
#if SUPERNOVA
        releaseBufferLocks();
#endif
        return false;
    }

    if (mBindingsNeedValidate) {
        if (onda_prepare_unchecked_process(mInstance) != 0) {
            Print(
                "ERROR: Onda (definition %d, instance %d): failed to prepare bindings for unchecked processing.\n",
                mDefinitionId,
                mUnitIndex);
#if SUPERNOVA
            releaseBufferLocks();
#endif
            return false;
        }
        mBindingsNeedValidate = false;
    }

    const int processResult = onda_process_unchecked(mInstance, executionOutput());
    flushExecutionOutput();
    
#if SUPERNOVA
    releaseBufferLocks();
#endif

    if (processResult != 0) {
        Print("ERROR: Onda (definition %d, instance %d): onda_process_unchecked failed.\n", mDefinitionId, mUnitIndex);
        return false;
    }

    if (mNeedsOutputCopy) {
        copyOutputsToSC(nSamples);
    }
    
    return true;
}

Onda::Onda() {
    mUnitIndex = mParentIndex;
    mDefinitionId = definitionIdFromControl(in0(0));

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
        silenceBlockOutputs(nSamples);
        setSilence();
        return;
    }

    if (!processAudio(nSamples)) {
        silenceBlockOutputs(nSamples);
        setSilence();
    }
}

void Onda::nextSilence(int nSamples) {
    silenceBlockOutputs(nSamples);
}

PluginLoad(OndaUGens) {
    ft = inTable;
    registerUnit<Onda>(ft, "Onda", true); // Onda assumes no aliasing between buffers
    Print("OndaCollider %s: using Onda %s.\n", ONDACOLLIDER_VERSION, ONDACOLLIDER_ONDA_VERSION);

    Onda::patchStorage.resize(kMaxDefinitionId);

    ft->fDefinePlugInCmd("onda_compile", ondaCompile, nullptr);
    ft->fDefinePlugInCmd("onda_free", ondaFree, nullptr);
}


#include "Onda.hpp"

#include <algorithm>
#include <cctype>
#include <cmath>
#include <cstdio>
#include <cstring>
#include <filesystem>
#include <fstream>
#include <limits>
#include <memory>
#include <new>
#include <sstream>
#include <string>
#include <type_traits>
#include <unordered_set>
#include <utility>
#include <vector>

static InterfaceTable* ft;

std::vector<PatchEntry> Onda::patchStorage;

namespace {

constexpr int kMaxReplySize = 4096;
constexpr int kMaxDefinitionId = 65536;
constexpr int kMaxPreallocatedInstances = 4096;
constexpr size_t kMaxPathBytes = 16384;
// Mirror Onda 0.7's editable-project transport limits so oversized snapshots are
// rejected before the host allocates their contents.
constexpr uintmax_t kMiB = UINTMAX_C(1024) * UINTMAX_C(1024);
constexpr uintmax_t kMaxProjectDocuments = 4096;
constexpr uintmax_t kMaxProjectAssets = 4096;
constexpr uintmax_t kOndaBufferHeaderBytes = 64;
constexpr uintmax_t kMaxProjectFileCount = kMaxProjectDocuments + kMaxProjectAssets + 1;
constexpr uintmax_t kMaxProjectFileBytes = UINTMAX_C(1024) * kMiB + kOndaBufferHeaderBytes;
constexpr uintmax_t kMaxProjectTotalBytes = UINTMAX_C(64) * kMiB
    + UINTMAX_C(64) * kMiB
    + UINTMAX_C(2048) * kMiB
    + kMaxProjectAssets * kOndaBufferHeaderBytes;

class ScopedOndaDiagnostic {
public:
    ScopedOndaDiagnostic() = default;
    ScopedOndaDiagnostic(const ScopedOndaDiagnostic&) = delete;
    ScopedOndaDiagnostic& operator=(const ScopedOndaDiagnostic&) = delete;

    ~ScopedOndaDiagnostic() { onda_diag_dispose(&mDiagnostic); }

    onda_diag_t* get() { return &mDiagnostic; }
    onda_diag_t* operator->() { return &mDiagnostic; }

    void reset() { onda_diag_dispose(&mDiagnostic); }

private:
    onda_diag_t mDiagnostic{};
};

template <typename T>
T* rtAllocateObjects(World* world, int count) {
    static_assert(std::is_nothrow_default_constructible<T>::value, "RT state construction must not throw");
    static_assert(std::is_trivially_destructible<T>::value, "RT state destruction must be trivial");
    if (count <= 0) {
        return nullptr;
    }

    auto* objects = static_cast<T*>(RTAlloc(world, static_cast<size_t>(count) * sizeof(T)));
    if (!objects) {
        return nullptr;
    }
    for (int i = 0; i < count; ++i) {
        new (objects + i) T{};
    }
    return objects;
}

struct ProjectFileStorage {
    std::filesystem::path absolutePath;
    std::string relativePath;
    uintmax_t expectedBytes = 0;
    std::filesystem::file_time_type expectedWriteTime;
    std::vector<uint8_t> bytes;
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

bool readProjectFile(
    ProjectFileStorage& file,
    std::string& error) {
    if (file.expectedBytes > static_cast<uintmax_t>(std::numeric_limits<size_t>::max())
        || file.expectedBytes > static_cast<uintmax_t>(std::numeric_limits<std::streamsize>::max())) {
        error = "Project file is too large for this host: '" + file.absolutePath.u8string() + "'.";
        return false;
    }

    std::ifstream stream(file.absolutePath, std::ios::binary);
    if (!stream) {
        error = "Failed to open project file '" + file.absolutePath.u8string() + "'.";
        return false;
    }

    file.bytes.resize(static_cast<size_t>(file.expectedBytes));
    if (!file.bytes.empty()) {
        stream.read(
            reinterpret_cast<char*>(file.bytes.data()),
            static_cast<std::streamsize>(file.bytes.size()));
        if (stream.gcount() != static_cast<std::streamsize>(file.bytes.size())) {
            error = "Project file changed while being read: '" + file.absolutePath.u8string() + "'.";
            return false;
        }
    }

    std::error_code ec;
    const uintmax_t finalBytes = std::filesystem::file_size(file.absolutePath, ec);
    if (ec
        || finalBytes != file.expectedBytes
        || std::filesystem::last_write_time(file.absolutePath, ec) != file.expectedWriteTime
        || ec) {
        error = "Project file changed while taking the compile snapshot: '"
            + file.absolutePath.u8string() + "'.";
        return false;
    }

    return true;
}

PatchEntry* patchEntryForDefinition(int definitionId) {
    if (definitionId < 1 || definitionId > static_cast<int>(Onda::patchStorage.size())) {
        return nullptr;
    }

    return &Onda::patchStorage[static_cast<size_t>(definitionId - 1)];
}

onda_program_t* compileProjectFile(
    const std::filesystem::path& manifestPath,
    const onda_compile_options_t& compileOptions,
    std::unordered_set<std::string>& projectBufferDefaults,
    ScopedOndaDiagnostic& diag,
    std::string& error) {
    std::error_code ec;
    const std::filesystem::path canonicalManifest = std::filesystem::canonical(manifestPath, ec);
    if (ec) {
        error = "Failed to resolve Onda project '" + manifestPath.u8string() + "': " + ec.message();
        return nullptr;
    }

    const std::filesystem::path projectRoot = canonicalManifest.parent_path();
    std::vector<ProjectFileStorage> storage;
    uintmax_t totalBytes = 0;

    try {
        for (const auto& entry : std::filesystem::recursive_directory_iterator(projectRoot)) {
            if (entry.is_symlink() || !entry.is_regular_file()) {
                continue;
            }

            if (storage.size() >= kMaxProjectFileCount) {
                error = "Onda project contains too many files.";
                return nullptr;
            }

            ProjectFileStorage file;
            file.absolutePath = entry.path();
            file.relativePath = entry.path().lexically_relative(projectRoot).generic_u8string();
            file.expectedBytes = entry.file_size();
            file.expectedWriteTime = entry.last_write_time();
            if (file.expectedBytes > kMaxProjectFileBytes) {
                error = "Onda project file exceeds the transport limit: '" + file.relativePath + "'.";
                return nullptr;
            }
            if (file.expectedBytes > kMaxProjectTotalBytes - totalBytes) {
                error = "Onda project exceeds the aggregate transport limit.";
                return nullptr;
            }
            totalBytes += file.expectedBytes;
            storage.push_back(std::move(file));
        }
    } catch (const std::filesystem::filesystem_error& filesystemError) {
        error = "Failed to read Onda project directory '" + projectRoot.u8string() + "': "
            + filesystemError.what();
        return nullptr;
    }

    std::sort(storage.begin(), storage.end(), [](const auto& lhs, const auto& rhs) {
        return lhs.relativePath < rhs.relativePath;
    });

    for (auto& file : storage) {
        if (!readProjectFile(file, error)) {
            return nullptr;
        }
    }

    std::vector<onda_project_file_t> files;
    files.reserve(storage.size());
    for (const auto& file : storage) {
        files.push_back({
            file.relativePath.c_str(),
            file.bytes.empty() ? nullptr : file.bytes.data(),
            file.bytes.size(),
        });
    }

    const std::string selectedManifest = canonicalManifest.lexically_relative(projectRoot).generic_u8string();
    onda_project_image_t* rawImage = onda_project_image_load_files(
        files.data(),
        files.size(),
        selectedManifest.c_str(),
        diag.get());
    if (!rawImage) {
        return nullptr;
    }
    std::unique_ptr<onda_project_image_t, decltype(&onda_project_image_destroy)> image(
        rawImage,
        onda_project_image_destroy);

    const int bufferCount = onda_project_image_buffer_count(image.get());
    if (bufferCount < 0) {
        error = "Failed to query buffer bindings from Onda project image.";
        return nullptr;
    }
    for (int i = 0; i < bufferCount; ++i) {
        if (const char* name = onda_project_image_buffer_name(image.get(), i)) {
            projectBufferDefaults.emplace(name);
        }
    }

    diag.reset();
    return onda_project_image_compile(image.get(), &compileOptions, diag.get());
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

bool buildProgramMetadata(
    CompiledProgram& compiled,
    bool compiledFromProject,
    const std::unordered_set<std::string>& projectBufferDefaults,
    std::string& error) {
    if (!compiled.program) {
        error = "Compiled program handle is null.";
        return false;
    }

    const int bufferCount = onda_buffer_count(compiled.program);
    const int inputCount = onda_input_count(compiled.program);
    const int paramCount = onda_param_count(compiled.program);
    const int eventCount = onda_event_count(compiled.program);
    const int outputCount = onda_output_count(compiled.program);
    const int bufferArrayCount = onda_buffer_array_count(compiled.program);

    if (inputCount < 0
        || paramCount < 0
        || eventCount < 0
        || bufferCount < 0
        || outputCount < 0
        || bufferArrayCount < 0) {
        error = "Failed to query endpoint counts from Onda program.";
        return false;
    }

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
        if (eventParamCount != 1) {
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

        const int payloadBytes = onda_event_payload_bytes(compiled.program, i);
        const int elemType = onda_event_param_elem_type(compiled.program, i, 0);
        const int arrayLen = onda_event_param_array_len(compiled.program, i, 0);
        const int isSlice = onda_event_param_is_slice(compiled.program, i, 0);

        if (payloadBytes != static_cast<int>(sizeof(float))
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
        // Event controls use SC trigger semantics. Zero is idle; a positive edge triggers the
        // event and the positive edge value is the single f32 payload.
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
        const bool hasProjectDefault = projectBufferDefaults.find(name) != projectBufferDefaults.end();
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
        desc.init = hasProjectDefault ? -1.0f : 0.0f;
        desc.bufferChannelsKind = channelsKind;
        desc.bufferChannelsStatic = channelsStatic;
        desc.bufferMayWrite = (mayWrite > 0);
        desc.hasProjectDefault = hasProjectDefault;

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
    int numAllocate = 0;
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
        for (const auto& slot : publish.replacedProgram->instances) {
            Onda* unit = slot.unit;
            if (unit) {
                unit->handleHotSwap(cmdData->definitionId, publishedProgram);
            }
        }
    }

    // Program is now owned by patch storage.
    cmdData->newProgram = nullptr;
    return true;
}

// NRT stage 2 (worker thread): compile source and preallocate Onda instances.
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
    if (cmdData->numAllocate < 1 || cmdData->numAllocate > kMaxPreallocatedInstances) {
        Print(
            "ERROR: Onda: numAllocate must be between 1 and %d (received %d).\n",
            kMaxPreallocatedInstances,
            cmdData->numAllocate);
        writeFailureReply(*cmdData);
        return true;
    }

    const std::filesystem::path filePath = std::filesystem::u8path(cmdData->path);
    std::error_code ec;
    
    const bool isFile = std::filesystem::exists(filePath, ec)
            && std::filesystem::is_regular_file(filePath, ec);
    
    if (!isFile) {
        Print("ERROR: Onda: failed to read source or project file '%s'.\n", cmdData->path);
        writeFailureReply(*cmdData);
        return true;
    }

    onda_compile_options_t compileOptions{};
    compileOptions.fast_math = 0;
    compileOptions.sample_rate = static_cast<float>(world->mSampleRate);
    compileOptions.block_size = world->mBufLength;

    ScopedOndaDiagnostic diag;
    std::string loadError;
    std::unordered_set<std::string> projectBufferDefaults;
    std::unique_ptr<CompiledProgram, decltype(&destroyCompiledProgram)> compiled(
        new CompiledProgram(),
        destroyCompiledProgram);
    const bool compiledFromProject = isProjectPath(filePath);
    if (compiledFromProject) {
        compiled->program = compileProjectFile(
            filePath,
            compileOptions,
            projectBufferDefaults,
            diag,
            loadError);
    } else {
        compiled->program = onda_compile_file(cmdData->path, &compileOptions, nullptr, diag.get());
    }

    if (!compiled->program) {
        const char* message = !loadError.empty()
            ? loadError.c_str()
            : (diag->message ? diag->message : "unknown compile error");
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
    if (!buildProgramMetadata(*compiled, compiledFromProject, projectBufferDefaults, metaError)) {
        Print("ERROR: Onda: %s\n", metaError.c_str());
        writeFailureReply(*cmdData);
        return true;
    }

    const int preallocateCount = cmdData->numAllocate;
    compiled->instances.reserve(static_cast<size_t>(preallocateCount));
    for (int i = 0; i < preallocateCount; ++i) {
        ScopedOndaDiagnostic instanceDiag;
        onda_instance_t* instance = onda_instance_create(
            compiled->program,
            compiled->requiredInputChannels,
            (compiled->requiredOutputChannels > 0) ? compiled->requiredOutputChannels : 1,
            instanceDiag.get());

        if (!instance) {
            const char* msg = instanceDiag->message ? instanceDiag->message : "unknown instance creation error";
            Print("ERROR: Onda: failed to preallocate instance %d/%d for definition %d: %s\n", i + 1, preallocateCount, cmdData->definitionId, msg);
            writeFailureReply(*cmdData);
            return true;
        }

        CompiledProgram::PreallocatedInstance slot;
        slot.instance = instance;
        slot.unit = nullptr;
        compiled->instances.push_back(slot);
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
        "Onda: compiled definition %d generation %d (%d inputs, %d flattened outputs, %d preallocated instances).\n",
        cmdData->definitionId,
        cmdData->generation,
        static_cast<int>(cmdData->newProgram->inputs.size()),
        cmdData->newProgram->requiredOutputChannels,
        static_cast<int>(cmdData->newProgram->instances.size()));

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
    cmdData->numAllocate = args->geti();
    
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
        for (const auto& slot : cmdData->programToDelete->instances) {
            Onda* unit = slot.unit;
            if (unit) {
                unit->handleFree(cmdData->definitionId);
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

    onda_instance_t* claimedInstance = nullptr;
    int claimedIndex = -1;
    if (!claimInstance(program, claimedInstance, claimedIndex)) {
        Print(
            "ERROR: Onda (definition %d): preallocated instances exhausted. Increase OndaDef numAllocate.\n",
            mDefinitionId);
        return false;
    }

    const onda_instance_t* expected = (claimedIndex >= 0 && claimedIndex < static_cast<int>(program->instances.size()))
        ? program->instances[static_cast<size_t>(claimedIndex)].instance
        : nullptr;

    if (expected != claimedInstance || !claimedInstance) {
        Print("ERROR: Onda (definition %d, instance %d): invalid runtime instance claim state.\n", mDefinitionId, claimedIndex);
        if (claimedIndex >= 0 && claimedIndex < static_cast<int>(program->instances.size())) {
            program->instances[static_cast<size_t>(claimedIndex)].unit = nullptr;
        }
        return false;
    }

    if (onda_reset_instance_state(claimedInstance) != 0) {
        Print("ERROR: Onda (definition %d, instance %d): failed to reset runtime instance.\n", mDefinitionId, claimedIndex);
        program->instances[static_cast<size_t>(claimedIndex)].unit = nullptr;
        return false;
    }

    if (!allocateRtState(program, claimedIndex)) {
        program->instances[static_cast<size_t>(claimedIndex)].unit = nullptr;
        return false;
    }

    releaseInstance();

    mProgram = program;
    mInstance = claimedInstance;
    mInstanceSlot = claimedIndex;

    mCalcFunc = make_calc_function<Onda, &Onda::next>();

    if (isHotSwap) {
        Print("Onda (definition %d, instance %d): hot-swapped.\n", mDefinitionId, mInstanceSlot);
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

bool Onda::allocateRtState(CompiledProgram* program, int instanceSlot) {
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

    auto releasePendingState = [&] {
        auto release = [&](auto*& pointer) {
            if (pointer) {
                RTFree(mWorld, pointer);
                pointer = nullptr;
            }
        };

        release(newAudioInputDescIndices);
        release(newParamDescIndices);
        release(newEventDescIndices);
        release(newBufferDescIndices);
        release(newScInputSlotByDesc);
        release(newRuntimeInputs);
        release(newRuntimeBuffers);
        release(newRuntimeOutputs);
        release(newOutputScratchBlock);
#if SUPERNOVA
        release(newBufferLocks);
#endif
    };

    const int scInputCount = numInputs() - 1;
    if (newBoundInputCount != scInputCount) {
        Print(
            "ERROR: Onda (definition %d, instance %d): program requires exactly %d SC inputs but UGen has %d. "
            "Relaunch the UGen with the new IO shape.\n",
            mDefinitionId,
            instanceSlot,
            newBoundInputCount,
            scInputCount);
        return false;
    }

    if (newBoundInputCount > 0) {
        newScInputSlotByDesc = static_cast<int*>(RTAlloc(mWorld, static_cast<size_t>(newBoundInputCount) * sizeof(int)));
        if (!newScInputSlotByDesc) {
            Print("ERROR: Onda (definition %d, instance %d): failed to allocate input slot map.\n", mDefinitionId, instanceSlot);
            return false;
        }

        for (int i = 0; i < newBoundInputCount; ++i) {
            newScInputSlotByDesc[i] = i + 1;
        }

        newRuntimeInputs = rtAllocateObjects<RuntimeInputState>(mWorld, newBoundInputCount);
        if (!newRuntimeInputs) {
            Print("ERROR: Onda (definition %d, instance %d): failed to allocate input runtime state.\n", mDefinitionId, instanceSlot);
            releasePendingState();
            return false;
        }
        newRuntimeBuffers = rtAllocateObjects<RuntimeBufferState>(mWorld, newBoundInputCount);
        if (!newRuntimeBuffers) {
            Print("ERROR: Onda (definition %d, instance %d): failed to allocate buffer runtime state.\n", mDefinitionId, instanceSlot);
            releasePendingState();
            return false;
        }
    }

    if (program->requiredOutputChannels != numOutputs()) {
        Print(
            "ERROR: Onda (definition %d, instance %d): program requires exactly %d output channels but UGen has %d. "
            "Relaunch the UGen with the new IO shape.\n",
            mDefinitionId,
            instanceSlot,
            program->requiredOutputChannels,
            numOutputs());
        releasePendingState();
        return false;
    }

    if (newBoundOutputCount > 0) {
        newRuntimeOutputs = rtAllocateObjects<RuntimeOutputState>(mWorld, newBoundOutputCount);
        if (!newRuntimeOutputs) {
            Print("ERROR: Onda (definition %d, instance %d): failed to allocate output runtime state.\n", mDefinitionId, instanceSlot);
            releasePendingState();
            return false;
        }
        int usedScOuts = 0;
        size_t totalScratchBytes = 0;
        for (int i = 0; i < newBoundOutputCount; ++i) {
            const auto& outDesc = newBoundOutputs[i];
            auto& state = newRuntimeOutputs[i];

            state.scOffset = usedScOuts;
            state.directBind = outDesc.arrayLen == 1;
            usedScOuts += outDesc.arrayLen;

            if (!state.directBind) {
                const size_t scratchBytes = sizeof(float)
                    * static_cast<size_t>(outDesc.arrayLen)
                    * static_cast<size_t>(bufferSize());
                if (scratchBytes > static_cast<size_t>(std::numeric_limits<int>::max())
                    || scratchBytes > std::numeric_limits<size_t>::max() - totalScratchBytes) {
                    Print(
                        "ERROR: Onda (definition %d, instance %d): output scratch layout exceeds the host limit.\n",
                        mDefinitionId,
                        instanceSlot);
                    releasePendingState();
                    return false;
                }
                state.scratchBytes = static_cast<int>(scratchBytes);
                totalScratchBytes += scratchBytes;
                newNeedsOutputCopy = true;
            }
        }

        if (usedScOuts != program->requiredOutputChannels) {
            Print(
                "ERROR: Onda (definition %d, instance %d): internal output mapping mismatch (%d mapped, %d required).\n",
                mDefinitionId,
                instanceSlot,
                usedScOuts,
                program->requiredOutputChannels);
            releasePendingState();
            return false;
        }

        if (totalScratchBytes > 0) {
            newOutputScratchBlock = static_cast<uint8_t*>(RTAlloc(mWorld, totalScratchBytes));
            if (!newOutputScratchBlock) {
                Print("ERROR: Onda (definition %d, instance %d): failed to allocate output scratch block.\n", mDefinitionId, instanceSlot);
                releasePendingState();
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
        newBufferLocks = rtAllocateObjects<BufferLockState>(mWorld, newBufferLockCapacity);
        if (!newBufferLocks) {
            Print("ERROR: Onda (definition %d, instance %d): failed to allocate buffer lock state.\n", mDefinitionId, instanceSlot);
            releasePendingState();
            return false;
        }
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
            Print("ERROR: Onda (definition %d, instance %d): failed to allocate %s index map.\n", mDefinitionId, instanceSlot, label);
            return false;
        }

        return true;
    };

    if (!allocateIndexMap(newAudioInputDescCount, newAudioInputDescIndices, "audio-input")
        || !allocateIndexMap(newParamDescCount, newParamDescIndices, "param")
        || !allocateIndexMap(newEventDescCount, newEventDescIndices, "event")
        || !allocateIndexMap(newBufferDescCount, newBufferDescIndices, "buffer")) {
        releasePendingState();
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
            mInstanceSlot);
        releaseInstance();
        freeRtState();
        mProgram = nullptr;
        setSilence();
    }
}

void Onda::handleFree(int definitionId) {
    if (definitionId != mDefinitionId) {
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
        const int bufIndex = scBufferIndex(rawBufNum);
        SndBuf* buf = resolveSndBufByIndex(bufIndex);

        if (isValidBufferBinding(buf, desc) && !addOrUpgradeBufferLock(buf, desc.bufferMayWrite)) {
            Print("ERROR: Onda (definition %d, instance %d): buffer lock state capacity exceeded.\n", mDefinitionId, mInstanceSlot);
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
        const int bufIndex = scBufferIndex(rawBufNum);
        SndBuf* buf = resolveSndBufByIndex(bufIndex);
        auto& state = mRuntimeBuffers[i];

        const bool valid = isValidBufferBinding(buf, desc);
        const bool useProjectDefault = !valid && desc.hasProjectDefault;
        allValid = allValid && (valid || useProjectDefault);

        if (useProjectDefault) {
            if (state.bound) {
                if (onda_reset_buffer_to_project_default(mInstance, desc.ondaIndex) != 0) {
                    Print(
                        "ERROR: Onda (definition %d, instance %d): failed to restore project buffer '%s'.\n",
                        mDefinitionId,
                        mInstanceSlot,
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
                mInstanceSlot,
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
                        mInstanceSlot,
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
                    mInstanceSlot,
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
        const int i = mAudioInputDescIndices[n];
        const auto& desc = mBoundInputs[i];

        const int scSlot = mScInputSlotByDesc[i];
        const float* src = (scSlot > 0) ? in(scSlot) : nullptr;

        if (!src) {
            Print(
                "ERROR: Onda (definition %d, instance %d): missing bound audio input '%s'. Relaunch the UGen with the new IO shape.\n",
                mDefinitionId,
                mInstanceSlot,
                desc.name.c_str());
            return false;
        }

        const void* bindPtr = src;
        const int bytes = static_cast<int>(sizeof(float) * static_cast<size_t>(bufferSize()));
        auto& state = mRuntimeInputs[i];
        const bool needsRebind = !state.bound || state.boundPtr != bindPtr || state.boundBytes != bytes;
        if (needsRebind) {
            if (onda_bind_input(mInstance, desc.ondaIndex, bindPtr, bytes) != 0) {
                Print("ERROR: Onda (definition %d, instance %d): failed to bind input '%s'.\n", mDefinitionId, mInstanceSlot, desc.name.c_str());
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
        auto& state = mRuntimeInputs[i];

        if (!state.controlInitialized || state.previousControl != value) {
            if (onda_set_param_by_index(mInstance, desc.ondaIndex, &value, static_cast<int>(sizeof(float))) != 0) {
                Print("ERROR: Onda (definition %d, instance %d): failed to set param '%s'.\n", mDefinitionId, mInstanceSlot, desc.name.c_str());
                return false;
            }
            state.previousControl = value;
            state.controlInitialized = true;
        }
    }

    for (int n = 0; n < mEventDescCount; ++n) {
        const int i = mEventDescIndices[n];
        const auto& desc = mBoundInputs[i];
        const int scSlot = mScInputSlotByDesc[i];
        const float value = (scSlot > 0) ? in0(scSlot) : 0.0f;
        auto& state = mRuntimeInputs[i];
        const bool triggered = value > 0.0f
            && (!state.controlInitialized || state.previousControl <= 0.0f);
        state.previousControl = value;
        state.controlInitialized = true;

        if (triggered
            && onda_trigger_event_by_index(mInstance, desc.ondaIndex, &value, static_cast<int>(sizeof(float))) != 0) {
            Print("ERROR: Onda (definition %d, instance %d): failed to trigger event '%s'.\n", mDefinitionId, mInstanceSlot, desc.name.c_str());
            return false;
        }
    }

    return true;
}

bool Onda::prepareOutputs() {
    for (int i = 0; i < mBoundOutputCount; ++i) {
        const auto& outDesc = mBoundOutputs[i];
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
                Print("ERROR: Onda (definition %d, instance %d): failed to bind output '%s'.\n", mDefinitionId, mInstanceSlot, outDesc.name.c_str());
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
            mInstanceSlot,
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

    if (!prepareParamsAndEvents()) {
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
                mInstanceSlot);
#if SUPERNOVA
            releaseBufferLocks();
#endif
            return false;
        }
        mBindingsNeedValidate = false;
    }

    const int processResult = onda_process_unchecked(mInstance);
    
#if SUPERNOVA
    releaseBufferLocks();
#endif

    if (processResult != 0) {
        Print("ERROR: Onda (definition %d, instance %d): onda_process_unchecked failed.\n", mDefinitionId, mInstanceSlot);
        return false;
    }

    if (mNeedsOutputCopy) {
        copyOutputsToSC(nSamples);
    }
    
    return true;
}

Onda::Onda() {
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
    Print("OndaCollider: using Onda %s.\n", ONDACOLLIDER_ONDA_VERSION);

    Onda::patchStorage.resize(kMaxDefinitionId);

    ft->fDefinePlugInCmd("onda_compile", ondaCompile, nullptr);
    ft->fDefinePlugInCmd("onda_free", ondaFree, nullptr);
}

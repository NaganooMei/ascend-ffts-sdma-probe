#include <acl/acl.h>

#if __has_include("runtime/rt_ffts_plus.h")
#include "runtime/rt_ffts_plus.h"
#elif __has_include("rt_external_ffts.h")
#include "rt_external_ffts.h"
#else
#error "FFTS Plus header was not found. Configure Ascend FFTS include directories in CMake."
#endif

#include <algorithm>
#include <chrono>
#include <cctype>
#include <cstdint>
#include <cstdlib>
#include <cstring>
#include <functional>
#include <iomanip>
#include <iostream>
#include <limits>
#include <stdexcept>
#include <string>
#include <vector>

#if defined(_WIN32)
#include <malloc.h>
#else
#include <sys/mman.h>
#endif

namespace {

constexpr uint32_t kFftsSdmaFp32AtomicMoveSqe = 0x1E70;
constexpr uint16_t kFftsContextMaxNum = 128;
constexpr uint8_t kFftsCommunicationTask = 0x5A;

static_assert(sizeof(rtFftsPlusComCtx_t) == 128, "rtFftsPlusComCtx_t must be 128 bytes");
static_assert(sizeof(rtFftsPlusSdmaCtx_t) == 128, "rtFftsPlusSdmaCtx_t must be 128 bytes");

enum class Mode {
    D2DSdma,
    H2DSdma,
    H2HSdma,
    All,
};

enum class HostMemoryKind {
    RegisteredMapped,
    MmapRegisteredMapped,
    AclrtRegisteredMapped,
};

struct Options {
    int32_t device{0};
    Mode mode{Mode::All};
    size_t bytes{1024 * 1024};
    uint16_t frags{1};
    uint16_t lanes{8};
    int32_t warmup{1};
    int32_t repeat{10};
    HostMemoryKind hostMemory{HostMemoryKind::AclrtRegisteredMapped};
};

struct CopySpec {
    void* dst{nullptr};
    const void* src{nullptr};
    size_t size{0};
};

[[noreturn]] void Fail(const std::string& message)
{
    throw std::runtime_error(message);
}

void CheckAcl(aclError ret, const char* expr)
{
    if (ret == ACL_SUCCESS) {
        return;
    }
    Fail(std::string(expr) + " failed, ret=" + std::to_string(static_cast<int32_t>(ret)));
}

template <typename Ret>
void CheckRt(Ret ret, const char* expr)
{
    if (ret == RT_ERROR_NONE) {
        return;
    }
    Fail(std::string(expr) + " failed, ret=" + std::to_string(static_cast<int32_t>(ret)));
}

uint64_t PtrToU64(const void* ptr)
{
    return static_cast<uint64_t>(reinterpret_cast<uintptr_t>(ptr));
}

uint8_t* AddBytes(void* ptr, size_t offset)
{
    return static_cast<uint8_t*>(ptr) + offset;
}

const uint8_t* AddBytes(const void* ptr, size_t offset)
{
    return static_cast<const uint8_t*>(ptr) + offset;
}

std::string ModeName(Mode mode)
{
    switch (mode) {
        case Mode::D2DSdma:
            return "d2d-sdma";
        case Mode::H2DSdma:
            return "h2d-sdma";
        case Mode::H2HSdma:
            return "h2h-sdma";
        case Mode::All:
            return "all";
    }
    return "unknown";
}

Mode ParseMode(const std::string& text)
{
    if (text == "d2d-sdma") {
        return Mode::D2DSdma;
    }
    if (text == "h2d-sdma") {
        return Mode::H2DSdma;
    }
    if (text == "h2h-sdma") {
        return Mode::H2HSdma;
    }
    if (text == "all") {
        return Mode::All;
    }
    Fail("invalid --mode: " + text);
}

std::string HostMemoryName(HostMemoryKind kind)
{
    switch (kind) {
        case HostMemoryKind::RegisteredMapped:
            return "registered-mapped";
        case HostMemoryKind::MmapRegisteredMapped:
            return "mmap-registered-mapped";
        case HostMemoryKind::AclrtRegisteredMapped:
            return "aclrt-registered-mapped";
    }
    return "unknown";
}

HostMemoryKind ParseHostMemory(const std::string& text)
{
    if (text == "registered-mapped") {
        return HostMemoryKind::RegisteredMapped;
    }
    if (text == "mmap-registered-mapped") {
        return HostMemoryKind::MmapRegisteredMapped;
    }
    if (text == "aclrt-registered-mapped") {
        return HostMemoryKind::AclrtRegisteredMapped;
    }
    Fail("invalid --host-mem: " + text);
}

bool UsesAclrtHostAllocation(HostMemoryKind kind)
{
    return kind == HostMemoryKind::AclrtRegisteredMapped;
}

bool NeedsPinnedRegistration(HostMemoryKind kind)
{
    return kind == HostMemoryKind::RegisteredMapped ||
           kind == HostMemoryKind::MmapRegisteredMapped;
}

bool UsesMmapHostAllocation(HostMemoryKind kind)
{
    return kind == HostMemoryKind::MmapRegisteredMapped;
}

size_t RoundUp(size_t value, size_t alignment)
{
    if (alignment == 0) {
        Fail("invalid alignment");
    }
    if (value > std::numeric_limits<size_t>::max() - (alignment - 1)) {
        Fail("size overflow while aligning host allocation");
    }
    return ((value + alignment - 1) / alignment) * alignment;
}

bool IsAligned(const void* ptr, size_t alignment)
{
    return reinterpret_cast<uintptr_t>(ptr) % alignment == 0;
}

size_t ParseSize(const std::string& text)
{
    if (text.empty()) {
        Fail("empty size");
    }

    size_t suffixPos = text.size();
    uint64_t multiplier = 1;
    const char suffix = static_cast<char>(std::toupper(static_cast<unsigned char>(text.back())));
    if (suffix == 'K' || suffix == 'M' || suffix == 'G') {
        suffixPos = text.size() - 1;
        if (suffix == 'K') {
            multiplier = 1024ULL;
        } else if (suffix == 'M') {
            multiplier = 1024ULL * 1024ULL;
        } else {
            multiplier = 1024ULL * 1024ULL * 1024ULL;
        }
    }

    const auto number = std::stoull(text.substr(0, suffixPos));
    if (number == 0 || number > std::numeric_limits<size_t>::max() / multiplier) {
        Fail("invalid size: " + text);
    }
    return static_cast<size_t>(number * multiplier);
}

int32_t ParseInt32(const std::string& text, const char* name)
{
    const auto value = std::stoll(text);
    if (value < std::numeric_limits<int32_t>::min() ||
        value > std::numeric_limits<int32_t>::max()) {
        Fail(std::string("out-of-range ") + name + ": " + text);
    }
    return static_cast<int32_t>(value);
}

uint16_t ParseU16(const std::string& text, const char* name, bool allowZero = false)
{
    const auto value = std::stoull(text);
    if ((!allowZero && value == 0) || value > std::numeric_limits<uint16_t>::max()) {
        Fail(std::string("invalid ") + name + ": " + text);
    }
    return static_cast<uint16_t>(value);
}

size_t CheckedMul(size_t lhs, size_t rhs, const char* name)
{
    if (lhs != 0 && rhs > std::numeric_limits<size_t>::max() / lhs) {
        Fail(std::string("size overflow for ") + name);
    }
    return lhs * rhs;
}

size_t TotalBytes(size_t bytes, uint16_t frags)
{
    return CheckedMul(bytes, frags, "total bytes");
}

size_t TotalBytes(const Options& opt)
{
    return TotalBytes(opt.bytes, opt.frags);
}

void PrintUsage(const char* argv0)
{
    std::cout
        << "Usage: " << argv0 << " [options]\n"
        << "\n"
        << "Options:\n"
        << "  --device N             Ascend device id, default 0\n"
        << "  --mode MODE            d2d-sdma, h2d-sdma, h2h-sdma, or all, default all\n"
        << "  --bytes BYTES          bytes per SDMA IO, supports K/M/G suffix, default 1048576\n"
        << "  --frags N              number of independent SDMA IO descriptors, default 1\n"
        << "  --lanes N              max ready contexts, 0 means auto, default 8\n"
        << "  --warmup N             warmup iterations, default 1\n"
        << "  --repeat N             timed iterations, default 10\n"
        << "  --host-mem KIND        registered-mapped, mmap-registered-mapped,\n"
        << "                         or aclrt-registered-mapped, default aclrt-registered-mapped\n"
        << "  --help                 show this help\n";
}

Options ParseArgs(int argc, char** argv)
{
    Options opt;
    for (int i = 1; i < argc; ++i) {
        const std::string arg = argv[i];
        auto requireValue = [&](const char* name) -> std::string {
            if (i + 1 >= argc) {
                Fail(std::string("missing value for ") + name);
            }
            return argv[++i];
        };

        if (arg == "--help" || arg == "-h") {
            PrintUsage(argv[0]);
            std::exit(0);
        } else if (arg == "--device") {
            opt.device = ParseInt32(requireValue("--device"), "--device");
        } else if (arg == "--mode") {
            opt.mode = ParseMode(requireValue("--mode"));
        } else if (arg == "--bytes") {
            opt.bytes = ParseSize(requireValue("--bytes"));
        } else if (arg == "--frags") {
            opt.frags = ParseU16(requireValue("--frags"), "--frags");
        } else if (arg == "--lanes") {
            opt.lanes = ParseU16(requireValue("--lanes"), "--lanes", true);
        } else if (arg == "--warmup") {
            opt.warmup = ParseInt32(requireValue("--warmup"), "--warmup");
        } else if (arg == "--repeat") {
            opt.repeat = ParseInt32(requireValue("--repeat"), "--repeat");
        } else if (arg == "--host-mem") {
            opt.hostMemory = ParseHostMemory(requireValue("--host-mem"));
        } else {
            Fail("unknown argument: " + arg);
        }
    }

    (void)TotalBytes(opt);
    if (opt.warmup < 0) {
        Fail("--warmup must be >= 0");
    }
    if (opt.repeat <= 0) {
        Fail("--repeat must be > 0");
    }
    return opt;
}

class AclRuntime {
public:
    explicit AclRuntime(int32_t device) : device_(device)
    {
        CheckAcl(aclInit(nullptr), "aclInit");
        initialized_ = true;
        CheckAcl(aclrtSetDevice(device_), "aclrtSetDevice");
        deviceSet_ = true;
        CheckAcl(aclrtCreateStream(&stream_), "aclrtCreateStream");
    }

    ~AclRuntime()
    {
        if (stream_ != nullptr) {
            (void)aclrtDestroyStream(stream_);
        }
        if (deviceSet_) {
            (void)aclrtResetDevice(device_);
        }
        if (initialized_) {
            (void)aclFinalize();
        }
    }

    aclrtStream Stream() const
    {
        return stream_;
    }

private:
    int32_t device_{0};
    bool initialized_{false};
    bool deviceSet_{false};
    aclrtStream stream_{nullptr};
};

class DeviceBuffer {
public:
    DeviceBuffer() = default;
    explicit DeviceBuffer(size_t bytes)
    {
        Allocate(bytes);
    }

    ~DeviceBuffer()
    {
        if (ptr_ != nullptr) {
            (void)aclrtFree(ptr_);
        }
    }

    DeviceBuffer(const DeviceBuffer&) = delete;
    DeviceBuffer& operator=(const DeviceBuffer&) = delete;

    void Allocate(size_t bytes)
    {
        if (bytes == 0) {
            Fail("invalid device allocation size");
        }
        CheckAcl(aclrtMalloc(&ptr_, bytes, ACL_MEM_TYPE_HIGH_BAND_WIDTH), "aclrtMalloc");
        bytes_ = bytes;
    }

    void* Ptr() const
    {
        return ptr_;
    }

    size_t Bytes() const
    {
        return bytes_;
    }

private:
    void* ptr_{nullptr};
    size_t bytes_{0};
};

class AclrtHostBuffer {
public:
    explicit AclrtHostBuffer(size_t bytes)
    {
        if (bytes == 0) {
            Fail("invalid host allocation size");
        }
        CheckAcl(aclrtMallocHost(&ptr_, bytes), "aclrtMallocHost");
        bytes_ = bytes;
    }

    ~AclrtHostBuffer()
    {
        if (ptr_ != nullptr) {
            (void)aclrtFreeHost(ptr_);
        }
    }

    AclrtHostBuffer(const AclrtHostBuffer&) = delete;
    AclrtHostBuffer& operator=(const AclrtHostBuffer&) = delete;

    void* Ptr() const
    {
        return ptr_;
    }

    size_t Bytes() const
    {
        return bytes_;
    }

private:
    void* ptr_{nullptr};
    size_t bytes_{0};
};

class HostBuffer {
public:
    HostBuffer(size_t bytes, HostMemoryKind kind) : kind_(kind)
    {
        if (bytes == 0) {
            Fail("invalid host allocation size");
        }
        bytes_ = bytes;
        if (UsesAclrtHostAllocation(kind_)) {
            allocationBytes_ = RoundUp(bytes, kHostRegisterAlignment);
            CheckAcl(aclrtMallocHost(&ptr_, allocationBytes_), "aclrtMallocHost");
            Register();
        } else if (UsesMmapHostAllocation(kind_)) {
            allocationBytes_ = RoundUp(bytes, kHostRegisterAlignment);
#if defined(_WIN32)
            Fail("mmap-registered-mapped is not supported on Windows");
#else
            int flags = MAP_PRIVATE | MAP_ANONYMOUS;
#if defined(MAP_POPULATE)
            flags |= MAP_POPULATE;
#endif
            ptr_ = mmap(nullptr, allocationBytes_, PROT_READ | PROT_WRITE, flags, -1, 0);
            if (ptr_ == MAP_FAILED) {
                ptr_ = nullptr;
                Fail("mmap failed");
            }
#endif
            Register();
        } else {
            allocationBytes_ = RoundUp(bytes, kHostRegisterAlignment);
#if defined(_WIN32)
            ptr_ = _aligned_malloc(allocationBytes_, kHostRegisterAlignment);
            if (ptr_ == nullptr) {
                Fail("_aligned_malloc failed");
            }
#else
            if (posix_memalign(&ptr_, kHostRegisterAlignment, allocationBytes_) != 0) {
                Fail("posix_memalign failed");
            }
#endif
            Register();
        }
    }

    ~HostBuffer()
    {
        if (ptr_ == nullptr) {
            return;
        }
        if (registered_) {
            (void)aclrtHostUnregister(ptr_);
        }
        if (UsesAclrtHostAllocation(kind_)) {
            (void)aclrtFreeHost(ptr_);
        } else if (UsesMmapHostAllocation(kind_)) {
#if !defined(_WIN32)
            (void)munmap(ptr_, allocationBytes_);
#endif
        } else {
#if defined(_WIN32)
            _aligned_free(ptr_);
#else
            std::free(ptr_);
#endif
        }
    }

    HostBuffer(const HostBuffer&) = delete;
    HostBuffer& operator=(const HostBuffer&) = delete;

    void* Ptr() const
    {
        return ptr_;
    }

    void* FftsDescriptorPtr(const char* role) const
    {
        if (mappedDevicePtr_ == nullptr) {
            Fail("registered mapped host buffer has no mapped device pointer");
        }
        (void)role;
        return mappedDevicePtr_;
    }

    size_t Bytes() const
    {
        return bytes_;
    }

private:
    void Register()
    {
        if (!IsAligned(ptr_, kHostRegisterAlignment)) {
            Fail("registered host pointer is not 4K aligned");
        }
        void* mappedDevicePtr = nullptr;
#if defined(ACL_HOST_REG_MAPPED)
        uint32_t flag = ACL_HOST_REG_MAPPED;
#if defined(ACL_HOST_REG_PINNED)
        if (NeedsPinnedRegistration(kind_)) {
            flag |= ACL_HOST_REG_PINNED;
        }
#endif
        auto ret = aclrtHostRegisterV2(ptr_, allocationBytes_, flag);
        CheckAcl(ret, "aclrtHostRegisterV2");
        registered_ = true;
        ret = aclrtHostGetDevicePointer(ptr_, &mappedDevicePtr, 0);
        CheckAcl(ret, "aclrtHostGetDevicePointer");
#else
        auto ret = aclrtHostRegister(ptr_, allocationBytes_, ACL_HOST_REGISTER_MAPPED,
                                     &mappedDevicePtr);
        CheckAcl(ret, "aclrtHostRegister");
        registered_ = true;
#endif
        mappedDevicePtr_ = mappedDevicePtr;
    }

    static constexpr size_t kHostRegisterAlignment = 4096;

    void* ptr_{nullptr};
    void* mappedDevicePtr_{nullptr};
    size_t bytes_{0};
    size_t allocationBytes_{0};
    HostMemoryKind kind_{HostMemoryKind::AclrtRegisteredMapped};
    bool registered_{false};
};

uint8_t PatternAt(size_t index)
{
    return static_cast<uint8_t>((index * 131U + 17U) & 0xFFU);
}

void FillPattern(void* ptr, size_t bytes)
{
    auto* data = static_cast<uint8_t*>(ptr);
    for (size_t i = 0; i < bytes; ++i) {
        data[i] = PatternAt(i);
    }
}

void FillZero(void* ptr, size_t bytes)
{
    std::memset(ptr, 0, bytes);
}

void VerifyPattern(const void* ptr, size_t bytes, const std::string& modeName)
{
    const auto* data = static_cast<const uint8_t*>(ptr);
    for (size_t i = 0; i < bytes; ++i) {
        const auto expected = PatternAt(i);
        if (data[i] != expected) {
            Fail(modeName + " verify failed at byte " + std::to_string(i) +
                 ", expected=" + std::to_string(static_cast<int32_t>(expected)) +
                 ", actual=" + std::to_string(static_cast<int32_t>(data[i])));
        }
    }
}

void WriteDeviceForSetup(aclrtStream stream, void* dst, size_t dstMax, const void* src, size_t bytes)
{
    CheckAcl(aclrtMemcpyAsync(dst, dstMax, src, bytes, ACL_MEMCPY_HOST_TO_DEVICE, stream),
             "aclrtMemcpyAsync(setup)");
    CheckAcl(aclrtSynchronizeStream(stream), "aclrtSynchronizeStream(setup)");
}

void ReadDeviceForVerify(aclrtStream stream, void* dst, size_t dstMax, const void* src, size_t bytes)
{
    CheckAcl(aclrtMemcpyAsync(dst, dstMax, src, bytes, ACL_MEMCPY_DEVICE_TO_HOST, stream),
             "aclrtMemcpyAsync(readback)");
    CheckAcl(aclrtSynchronizeStream(stream), "aclrtSynchronizeStream(readback)");
}

std::vector<CopySpec> BuildSpecs(void* dstBase,
                                 const void* srcBase,
                                 size_t bytes,
                                 uint16_t frags)
{
    std::vector<CopySpec> specs;
    specs.reserve(frags);
    if (bytes > std::numeric_limits<uint32_t>::max()) {
        Fail("single SDMA IO size exceeds uint32_t limit");
    }

    for (uint16_t frag = 0; frag < frags; ++frag) {
        const size_t offset = CheckedMul(bytes, frag, "copy offset");
        specs.push_back({AddBytes(dstBase, offset), AddBytes(srcBase, offset), bytes});
    }
    return specs;
}

void BuildSdmaCtx(void* dst, const void* src, size_t size, rtFftsPlusSdmaCtx_t* ctx)
{
    constexpr uint32_t kShift = 32;
    constexpr uint64_t kLowMask = 0xFFFFFFFFULL;

    const uint64_t srcAddr = PtrToU64(src);
    const uint64_t dstAddr = PtrToU64(dst);

    ctx->contextType = RT_CTX_TYPE_SDMA;
    ctx->threadDim = 1;
    ctx->sdmaSqeHeader = kFftsSdmaFp32AtomicMoveSqe;
    ctx->sourceAddressBaseL = static_cast<uint32_t>(srcAddr & kLowMask);
    ctx->sourceAddressBaseH = static_cast<uint32_t>(srcAddr >> kShift);
    ctx->sourceAddressOffset = 0;
    ctx->destinationAddressBaseL = static_cast<uint32_t>(dstAddr & kLowMask);
    ctx->destinationAddressBaseH = static_cast<uint32_t>(dstAddr >> kShift);
    ctx->destinationAddressOffset = 0;
    ctx->nonTailDataLength = static_cast<uint32_t>(size);
    ctx->tailDataLength = static_cast<uint32_t>(size);
}

void AddDependency(std::vector<rtFftsPlusComCtx_t>& contexts,
                   uint32_t predecessorId,
                   uint32_t successorId)
{
    if (predecessorId >= contexts.size() || successorId >= contexts.size()) {
        Fail("invalid FFTS dependency");
    }
    auto& predecessor = contexts[predecessorId];
    auto& successor = contexts[successorId];
    if (predecessor.successorNum >= RT_CTX_SUCCESSOR_NUM ||
        successor.predCntInit >= std::numeric_limits<uint8_t>::max()) {
        Fail("too many FFTS dependencies");
    }

    predecessor.successorList[predecessor.successorNum] = static_cast<uint16_t>(successorId);
    predecessor.successorNum++;
    successor.predCntInit++;
    successor.predCnt++;
}

std::vector<rtFftsPlusComCtx_t> BuildContexts(const std::vector<CopySpec>& copies,
                                               uint16_t maxReadyLanes,
                                               uint16_t& readyContextNum)
{
    if (copies.empty()) {
        Fail("empty FFTS copy specs");
    }
    if (copies.size() > std::numeric_limits<uint16_t>::max()) {
        Fail("too many FFTS contexts");
    }

    std::vector<rtFftsPlusComCtx_t> contexts;
    contexts.reserve(copies.size());

    const auto requestedLanes = maxReadyLanes == 0 ? copies.size() : maxReadyLanes;
    const auto laneCount = static_cast<uint16_t>(std::min<size_t>(copies.size(), requestedLanes));
    std::vector<int32_t> lastTaskId(laneCount, -1);

    for (size_t i = 0; i < copies.size(); ++i) {
        const auto& copy = copies[i];
        if (copy.dst == nullptr || copy.src == nullptr || copy.size == 0) {
            Fail("invalid FFTS copy spec");
        }
        if (copy.size > std::numeric_limits<uint32_t>::max()) {
            Fail("FFTS copy size is too large");
        }

        rtFftsPlusComCtx_t comCtx{};
        auto* sdmaCtx = reinterpret_cast<rtFftsPlusSdmaCtx_t*>(&comCtx);
        BuildSdmaCtx(copy.dst, copy.src, copy.size, sdmaCtx);
        contexts.push_back(comCtx);

        const size_t lane = i % laneCount;
        const auto taskId = static_cast<uint32_t>(contexts.size() - 1);
        if (lastTaskId[lane] >= 0) {
            AddDependency(contexts, static_cast<uint32_t>(lastTaskId[lane]), taskId);
        }
        lastTaskId[lane] = static_cast<int32_t>(taskId);
    }

    readyContextNum = laneCount;
    return contexts;
}

void LaunchFfts(aclrtStream stream,
                std::vector<rtFftsPlusComCtx_t>& contexts,
                uint16_t readyContextNum)
{
    if (stream == nullptr) {
        Fail("invalid FFTS stream");
    }
    if (contexts.empty()) {
        Fail("empty FFTS contexts");
    }
    if (readyContextNum == 0 || readyContextNum > contexts.size()) {
        Fail("invalid FFTS ready context number");
    }

    rtFftsPlusSqe_t sqe{};
    sqe.fftsType = RT_FFTS_PLUS_TYPE;
    sqe.totalContextNum = static_cast<uint16_t>(contexts.size());
    sqe.readyContextNum = readyContextNum;
    sqe.preloadContextNum = std::min<uint16_t>(readyContextNum, kFftsContextMaxNum);
    sqe.timeout = 0;
    sqe.subType = kFftsCommunicationTask;

    rtFftsPlusTaskInfo_t task{};
    task.fftsPlusSqe = &sqe;
    task.descBuf = contexts.data();
    task.descBufLen = sizeof(rtFftsPlusComCtx_t) * contexts.size();
    task.descAddrType = RT_FFTS_PLUS_CTX_DESC_ADDR_TYPE_HOST;
    task.argsHandleInfoNum = 0;
    task.argsHandleInfoPtr = nullptr;

    CheckRt(rtFftsPlusTaskLaunchWithFlag(&task, reinterpret_cast<rtStream_t>(stream), 0),
            "rtFftsPlusTaskLaunchWithFlag");
}

double RunTimedFfts(aclrtStream stream,
                    const std::vector<CopySpec>& copies,
                    uint16_t lanes,
                    int32_t warmup,
                    int32_t repeat,
                    const std::function<void()>& resetDestination)
{
    uint16_t readyContextNum = 0;
    const auto contextTemplate = BuildContexts(copies, lanes, readyContextNum);

    double totalUs = 0.0;
    const int32_t iterations = warmup + repeat;
    for (int32_t i = 0; i < iterations; ++i) {
        resetDestination();
        auto contexts = contextTemplate;
        const auto start = std::chrono::steady_clock::now();
        LaunchFfts(stream, contexts, readyContextNum);
        CheckAcl(aclrtSynchronizeStream(stream), "aclrtSynchronizeStream(FFTS)");
        const auto end = std::chrono::steady_clock::now();
        if (i >= warmup) {
            totalUs +=
                std::chrono::duration<double, std::micro>(end - start).count();
        }
    }
    return totalUs / static_cast<double>(repeat);
}

void PrintConfig(const Options& opt)
{
    std::cout << "probe_config"
              << " mode=" << ModeName(opt.mode)
              << " device=" << opt.device
              << " bytes_per_io=" << opt.bytes
              << " total_bytes=" << TotalBytes(opt)
              << " frags=" << opt.frags
              << " lanes=" << opt.lanes
              << " warmup=" << opt.warmup
              << " repeat=" << opt.repeat
              << " host_mem=" << HostMemoryName(opt.hostMemory)
              << "\n";
}

void PrintResult(const std::string& modeName, size_t bytes, double avgUs)
{
    const double seconds = avgUs / 1000.0 / 1000.0;
    const double gib = static_cast<double>(bytes) / 1024.0 / 1024.0 / 1024.0;
    const double bandwidth = seconds > 0.0 ? gib / seconds : 0.0;

    std::cout << std::fixed << std::setprecision(3)
              << "result"
              << " mode=" << modeName
              << " status=PASS"
              << " avg_us=" << avgUs
              << " bandwidth_gib_s=" << bandwidth
              << "\n";
}

void RunD2DSdma(aclrtStream stream, const Options& opt)
{
    const size_t totalBytes = TotalBytes(opt);
    AclrtHostBuffer hostSource(totalBytes);
    AclrtHostBuffer hostZero(totalBytes);
    AclrtHostBuffer hostVerify(totalBytes);
    DeviceBuffer deviceSource(totalBytes);
    DeviceBuffer deviceDestination(totalBytes);

    FillPattern(hostSource.Ptr(), hostSource.Bytes());
    FillZero(hostZero.Ptr(), hostZero.Bytes());
    WriteDeviceForSetup(stream, deviceSource.Ptr(), deviceSource.Bytes(), hostSource.Ptr(),
                        totalBytes);

    const auto specs =
        BuildSpecs(deviceDestination.Ptr(), deviceSource.Ptr(), opt.bytes, opt.frags);
    const auto resetDestination = [&]() {
        WriteDeviceForSetup(stream, deviceDestination.Ptr(), deviceDestination.Bytes(),
                            hostZero.Ptr(), totalBytes);
    };
    const double avgUs =
        RunTimedFfts(stream, specs, opt.lanes, opt.warmup, opt.repeat, resetDestination);

    ReadDeviceForVerify(stream, hostVerify.Ptr(), hostVerify.Bytes(), deviceDestination.Ptr(),
                        totalBytes);
    VerifyPattern(hostVerify.Ptr(), totalBytes, "d2d-sdma");
    PrintResult("d2d-sdma", totalBytes, avgUs);
}

void RunH2DSdma(aclrtStream stream, const Options& opt)
{
    const size_t totalBytes = TotalBytes(opt);
    HostBuffer hostSource(totalBytes, opt.hostMemory);
    AclrtHostBuffer hostZero(totalBytes);
    AclrtHostBuffer hostVerify(totalBytes);
    DeviceBuffer deviceDestination(totalBytes);

    FillPattern(hostSource.Ptr(), hostSource.Bytes());
    FillZero(hostZero.Ptr(), hostZero.Bytes());
    const auto specs =
        BuildSpecs(deviceDestination.Ptr(), hostSource.FftsDescriptorPtr("src"), opt.bytes,
                   opt.frags);
    const auto resetDestination = [&]() {
        WriteDeviceForSetup(stream, deviceDestination.Ptr(), deviceDestination.Bytes(),
                            hostZero.Ptr(), totalBytes);
    };
    const double avgUs =
        RunTimedFfts(stream, specs, opt.lanes, opt.warmup, opt.repeat, resetDestination);

    ReadDeviceForVerify(stream, hostVerify.Ptr(), hostVerify.Bytes(), deviceDestination.Ptr(),
                        totalBytes);
    VerifyPattern(hostVerify.Ptr(), totalBytes, "h2d-sdma");
    PrintResult("h2d-sdma", totalBytes, avgUs);
}

void RunH2HSdma(aclrtStream stream, const Options& opt)
{
    const size_t totalBytes = TotalBytes(opt);
    HostBuffer hostSource(totalBytes, opt.hostMemory);
    HostBuffer hostDestination(totalBytes, opt.hostMemory);

    FillPattern(hostSource.Ptr(), hostSource.Bytes());
    FillZero(hostDestination.Ptr(), hostDestination.Bytes());
    const auto specs = BuildSpecs(hostDestination.FftsDescriptorPtr("dst"),
                                  hostSource.FftsDescriptorPtr("src"), opt.bytes, opt.frags);
    const auto resetDestination = [&]() {
        FillZero(hostDestination.Ptr(), hostDestination.Bytes());
    };
    const double avgUs =
        RunTimedFfts(stream, specs, opt.lanes, opt.warmup, opt.repeat, resetDestination);

    VerifyPattern(hostDestination.Ptr(), totalBytes, "h2h-sdma");
    PrintResult("h2h-sdma", totalBytes, avgUs);
}

}  // namespace

int main(int argc, char** argv)
{
    try {
        const auto opt = ParseArgs(argc, argv);
        PrintConfig(opt);

        AclRuntime runtime(opt.device);
        if (opt.mode == Mode::All || opt.mode == Mode::D2DSdma) {
            RunD2DSdma(runtime.Stream(), opt);
        }
        if (opt.mode == Mode::All || opt.mode == Mode::H2HSdma) {
            RunH2HSdma(runtime.Stream(), opt);
        }
        if (opt.mode == Mode::All || opt.mode == Mode::H2DSdma) {
            RunH2DSdma(runtime.Stream(), opt);
        }
        return 0;
    } catch (const std::exception& e) {
        std::cerr << "ERROR: " << e.what() << "\n";
        return 1;
    }
}

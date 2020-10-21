#pragma once

#include "base/common.h"
#include "utils/object_pool.h"
#include "utils/buffer_pool.h"

#include <liburing.h>

namespace faas {
namespace engine {

class IOUring {
public:
    explicit IOUring(int entries);
    ~IOUring();

    void PrepareBuffers(uint16_t gid, size_t buf_size);

    typedef std::function<bool(int /* status */, std::span<const char> /* data */)> ReadCallback;
    bool StartRead(int fd, uint16_t buf_gid, ReadCallback cb);
    bool StartRecv(int fd, uint16_t buf_gid, ReadCallback cb);
    bool StopReadOrRecv(int fd);

    // Partial write may happen. The caller is responsible for handling partial writes.
    typedef std::function<void(int /* status */, size_t /* nwrite */)> WriteCallback;
    bool Write(int fd, std::span<const char> data, WriteCallback cb);

    // Only works for sockets. Partial write will not happen.
    // IOUring implementation will correctly order all SendAll writes.
    typedef std::function<void(int /* status */)> SendAllCallback;
    bool SendAll(int sockfd, std::span<const char> data, SendAllCallback cb);

    typedef std::function<void()> CloseCallback;
    bool Close(int fd, CloseCallback cb);

    void EventLoopRunOnce(int* inflight_ops);

private:
    struct io_uring ring_;

    absl::flat_hash_map</* gid */ uint16_t, std::unique_ptr<utils::BufferPool>> buf_pools_;
    absl::flat_hash_map</* fd */ int, int> ref_counts_;

    enum OpType { kRead, kWrite, kSendAll, kClose, kCancel };
    enum {
        kOpFlagRepeat    = 1 << 0,
        kOpFlagUseRecv   = 1 << 1,
        kOpFlagCancelled = 1 << 2,
    };
    static constexpr uint64_t kInvalidOpId = ~0ULL;
    static constexpr int kInvalidFd = -1;
    struct Op {
        uint64_t id;         // Lower 8-bit stores type
        int      fd;         // Used by kRead, kWrite, kSendAll, kClose
        uint16_t buf_gid;    // Used by kRead
        uint16_t flags;
        char*    buf;        // Used by kRead, kWrite, kSendAll
        size_t   buf_len;    // Used by kRead, kWrite, kSendAll
        uint64_t next_op;    // Used by kSendAll
    };

    uint64_t next_op_id_;
    utils::SimpleObjectPool<Op> op_pool_;
    absl::flat_hash_map</* op_id */ uint64_t, Op*> ops_;
    absl::flat_hash_map</* fd */ int, Op*> read_ops_;
    absl::flat_hash_map</* op_id */ uint64_t, ReadCallback> read_cbs_;
    absl::flat_hash_map</* op_id */ uint64_t, WriteCallback> write_cbs_;
    absl::flat_hash_map</* op_id */ uint64_t, SendAllCallback> sendall_cbs_;
    absl::flat_hash_map</* fd */ int, Op*> last_send_op_;
    absl::flat_hash_map</* fd */ int, CloseCallback> close_cbs_;

    inline OpType op_type(const Op* op) { return gsl::narrow_cast<OpType>(op->id & 0xff); }

    bool StartReadInternal(int fd, uint16_t buf_gid, uint16_t flags, ReadCallback cb);

    Op* AllocReadOp(int fd, uint16_t buf_gid, std::span<char> buf, uint16_t flags);
    Op* AllocWriteOp(int fd, std::span<const char> data);
    Op* AllocSendAllOp(int fd, std::span<const char> data);
    Op* AllocCloseOp(int fd);
    Op* AllocCancelOp(uint64_t op_id);

    void EnqueueOp(Op* op);
    void OnOpComplete(Op* op, struct io_uring_cqe* cqe);
    void RefFd(int fd);
    void UnrefFd(int fd);

    void HandleReadOpComplete(Op* op, int res);
    void HandleWriteOpComplete(Op* op, int res);
    void HandleSendallOpComplete(Op* op, int res);

    DISALLOW_COPY_AND_ASSIGN(IOUring);
};

#define URING_CHECK_OK(URING_CALL)                    \
    do {                                              \
        bool ret = URING_CALL;                        \
        LOG_IF(FATAL, ret) << "IOUring call failed";  \
    } while (0)

#define URING_DCHECK_OK(URING_CALL)                   \
    do {                                              \
        bool ret = URING_CALL;                        \
        DLOG_IF(FATAL, ret) << "IOUring call failed"; \
    } while (0)

}  // namespace engine
}  // namespace faas

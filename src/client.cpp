#include <algorithm>
#include <sstream>
#include <iomanip>

#include "rpc.hpp"
#include "client.hpp"
#include "container.hpp"
#include "property.hpp"
#include "statistics.hpp"
#include "config.hpp"
#include "protobuf.hpp"
#include "util/log.hpp"
#include "util/string.hpp"
#include "portod.hpp"

#include <google/protobuf/io/coded_stream.h>

extern "C" {
#include <sys/types.h>
#include <sys/socket.h>
#include <sys/un.h>
#include <unistd.h>
};

TClient SystemClient("<system>");
__thread TClient *CurrentClient = nullptr;

TClient::TClient() : TEpollSource(-1) {
    ConnectionTime = GetCurrentTimeMs();
    Statistics->ClientsCount++;
}

TClient::TClient(const std::string &special) {
    Cred = TCred(RootUser, RootGroup);
    Comm = special;
    AccessLevel = EAccessLevel::Internal;
}

TClient::~TClient() {
    CloseConnection();
    if (AccessLevel != EAccessLevel::Internal)
        Statistics->ClientsCount--;
}

TError TClient::AcceptConnection(int listenFd) {
    struct sockaddr_un peer_addr;
    socklen_t peer_addr_size;
    TError error;

    peer_addr_size = sizeof(struct sockaddr_un);
    Fd = accept4(listenFd, (struct sockaddr *) &peer_addr,
                  &peer_addr_size, SOCK_NONBLOCK | SOCK_CLOEXEC);
    if (Fd < 0) {
        error = TError(EError::Unknown, errno, "accept4()");
        if (error.GetErrno() != EAGAIN)
            L_WRN() << "Cannot accept client: " << error << std::endl;
        return error;
    }

    error = IdentifyClient(true);
    if (error) {
        close(Fd);
        Fd = -1;
        return error;
    }

    if (Verbose)
        L() << "Client connected: " << *this << std::endl;

    return TError::Success();
}

void TClient::CloseConnection() {
    TScopedLock lock(Mutex);

    if (Fd >= 0) {
        EpollLoop->RemoveSource(Fd);
        ConnectionTime = GetCurrentTimeMs() - ConnectionTime;
        if (Verbose)
            L() << "Client disconnected: " << *this
                << " : " << ConnectionTime << " ms" <<  std::endl;
        close(Fd);
        Fd = -1;
    }

    for (auto &weakCt: WeakContainers) {
        auto container = weakCt.lock();
        if (container)
            container->DestroyWeak();
    }
    WeakContainers.clear();
}

void TClient::StartRequest() {
    RequestStartMs = GetCurrentTimeMs();
    PORTO_ASSERT(CurrentClient == nullptr);
    CurrentClient = this;
}

void TClient::FinishRequest() {
    ReleaseContainer();
    PORTO_ASSERT(CurrentClient == this);
    CurrentClient = nullptr;
}

uint64_t TClient::GetRequestTimeMs() {
    return GetCurrentTimeMs() - RequestStartMs;
}

TError TClient::IdentifyClient(bool initial) {
    std::shared_ptr<TContainer> ct;
    struct ucred cr;
    socklen_t len = sizeof(cr);
    TError error;

    if (getsockopt(Fd, SOL_SOCKET, SO_PEERCRED, &cr, &len))
        return TError(EError::Unknown, errno, "Cannot identify client: getsockopt() failed");

    /* check that request from the same pid and container is still here */
    if (!initial && Pid == cr.pid && TaskCred.Uid == cr.uid &&
            TaskCred.Gid == cr.gid && ClientContainer &&
            (ClientContainer->State == EContainerState::Running ||
             ClientContainer->State == EContainerState::Meta))
        return TError::Success();

    TaskCred.Uid = cr.uid;
    TaskCred.Gid = cr.gid;
    Pid = cr.pid;

    error = TContainer::FindTaskContainer(Pid, ct);
    if (error && error.GetErrno() != ENOENT)
        L_WRN() << "Cannot identify container of pid " << Pid
                << " : " << error << std::endl;
    if (error)
        return error;

    AccessLevel = ct->AccessLevel;
    for (auto p = ct->Parent; p; p = p->Parent)
        AccessLevel = std::min(AccessLevel, p->AccessLevel);

    if (AccessLevel == EAccessLevel::None)
        return TError(EError::Permission, "Porto disabled in container " + ct->Name);

    if (ct->State != EContainerState::Running && ct->State != EContainerState::Meta)
        return TError(EError::Permission, "Client from containers in state " + TContainer::StateName(ct->State));

    ClientContainer = ct;

    error = TPath("/proc/" + std::to_string(Pid) + "/comm").ReadAll(Comm, 64);
    if (error)
        Comm = "<unknown process>";
    else
        Comm.resize(Comm.length() - 1); /* cut \n at the end */

    if (ct->IsRoot()) {
        Cred.Uid = cr.uid;
        Cred.Gid = cr.gid;
        error = LoadGroups();
        if (error && error.GetErrno() != ENOENT)
            L_WRN() << "Cannot load supplementary group list" << Pid
                    << " : " << error << std::endl;
    } else {
        /* requests from containers are executed in behalf of their owners */
        Cred = ct->OwnerCred;
    }

    if (Cred.IsRootUser()) {
        if (AccessLevel == EAccessLevel::Normal)
            AccessLevel = EAccessLevel::SuperUser;
    } else if (!Cred.IsMemberOf(PortoGroup)) {
        if (AccessLevel >= EAccessLevel::ReadOnly)
            AccessLevel = EAccessLevel::ReadOnly;
    }

    return TError::Success();
}

TError TClient::LoadGroups() {
    std::vector<std::string> lines;
    TError error = TPath("/proc/" + std::to_string(Pid) + "/status").ReadLines(lines);
    if (error)
        return error;

    Cred.Groups.clear();
    for (auto &l : lines)
        if (l.compare(0, 8, "Groups:\t") == 0) {
            std::vector<std::string> groupsStr;

            error = SplitString(l.substr(8), ' ', groupsStr);
            if (error)
                return error;

            for (auto g : groupsStr) {
                int group;
                error = StringToInt(g, group);
                if (error)
                    return error;

                Cred.Groups.push_back(group);
            }

            break;
        }

    return TError::Success();
}

TError TClient::ComposeName(const std::string &name, std::string &relative_name) const {
    std::string ns = ClientContainer->GetPortoNamespace();

    if (name == ROOT_CONTAINER) {
        relative_name = ROOT_CONTAINER;
        return TError::Success();
    }

    if (ns == "") {
        relative_name = name;
        return TError::Success();
    }

    if (!StringStartsWith(name, ns))
        return TError(EError::Permission,
                "Cannot access container " + name + " from namespace " + ns);

    relative_name = name.substr(ns.length());
    return TError::Success();
}

TError TClient::ResolveName(const std::string &relative_name, std::string &name) const {
    std::string ns = ClientContainer->GetPortoNamespace();

    if (relative_name == ROOT_CONTAINER)
        name = ROOT_CONTAINER;
    else if (relative_name == SELF_CONTAINER)
        name = ClientContainer->Name;
    else if (relative_name == DOT_CONTAINER)
        name = TContainer::ParentName(ns);
    else if (StringStartsWith(relative_name, SELF_CONTAINER + std::string("/")))
        name = (ClientContainer->IsRoot() ? "" : ClientContainer->Name) +
            relative_name.substr(strlen(SELF_CONTAINER));
    else if (StringStartsWith(relative_name, ROOT_PORTO_NAMESPACE)) {
        name = relative_name.substr(strlen(ROOT_PORTO_NAMESPACE));
        if (!StringStartsWith(name, ns))
            return TError(EError::Permission, "Absolute container name out of current namespace");
    } else
        name = ns + relative_name;

    return TError::Success();
}

TError TClient::ResolveContainer(const std::string &relative_name,
                                 std::shared_ptr<TContainer> &ct) const {
    std::string name;
    TError error = ResolveName(relative_name, name);
    if (error)
        return error;
    return TContainer::Find(name, ct);
}

TError TClient::ReadContainer(const std::string &relative_name,
                              std::shared_ptr<TContainer> &ct, bool try_lock) {
    auto lock = LockContainers();
    TError error = ResolveContainer(relative_name, ct);
    if (error)
        return error;
    ReleaseContainer(true);
    error = ct->LockRead(lock, try_lock);
    if (error)
        return error;
    LockedContainer = ct;
    return TError::Success();
}

TError TClient::WriteContainer(const std::string &relative_name,
                               std::shared_ptr<TContainer> &ct, bool child) {
    if (AccessLevel <= EAccessLevel::ReadOnly)
        return TError(EError::Permission, "No write access at all");
    auto lock = LockContainers();
    TError error = ResolveContainer(relative_name, ct);
    if (error)
        return error;
    error = CanControl(*ct, child);
    if (error)
        return error;
    ReleaseContainer(true);
    error = ct->Lock(lock);
    if (error)
        return error;
    LockedContainer = ct;
    return TError::Success();
}

void TClient::ReleaseContainer(bool locked) {
    if (LockedContainer) {
        LockedContainer->Unlock(locked);
        LockedContainer = nullptr;
    }
}

TPath TClient::ComposePath(const TPath &path) {
    return ClientContainer->RootPath.InnerPath(path);
}

TPath TClient::ResolvePath(const TPath &path) {
    return ClientContainer->RootPath / path;
}

bool TClient::IsSuperUser(void) const {
    return AccessLevel >= EAccessLevel::SuperUser;
}

TError TClient::CanControl(const TCred &other) {

    if (AccessLevel <= EAccessLevel::ReadOnly)
        return TError(EError::Permission, "No write access at all");

    if (IsSuperUser() || Cred.Uid == other.Uid)
        return TError::Success();

    /* Everybody can control users from group porto-containers */
    if (other.IsMemberOf(PortoCtGroup))
        return TError::Success();

    /* Load group $USER-containers lazily */
    if (!UserCtGroup && GroupId(Cred.User() + USER_CT_SUFFIX, UserCtGroup))
        UserCtGroup = NoGroup;

    if (other.IsMemberOf(UserCtGroup))
        return TError::Success();

    return TError(EError::Permission, "User " + Cred.ToString() +
                                      " cannot control " + other.ToString());
}

TError TClient::CanControl(const TContainer &ct, bool child) {

    if (AccessLevel < EAccessLevel::ChildOnly)
        return TError(EError::Permission, "No write access at all");

    if (!child && ct.IsRoot())
        return TError(EError::Permission, "Root container is read-only");

    if (!child || !ct.IsRoot()) {
        TError error = CanControl(ct.OwnerCred);
        if (error)
            return error;
    }

    if (AccessLevel > EAccessLevel::ChildOnly)
        return TError::Success();

    auto base = ClientContainer;
    while (base && base->AccessLevel != EAccessLevel::ChildOnly)
        base = base->Parent;
    if (!base)
        return TError(EError::Permission, "Base for child-only not found");

    if ((child && base.get() == &ct) || ct.IsChildOf(*base))
        return TError::Success();

    return TError(EError::Permission, "Not a child container: " + ct.Name);
}

TError TClient::ReadRequest(rpc::TContainerRequest &request) {
    TScopedLock lock(Mutex);

    if (Processing) {
        L_WRN() << "Client request before response: " << *this << std::endl;
        return TError::Success();
    }

    if (Fd < 0)
        return TError(EError::Unknown, "Connection closed");

    if (Offset >= Buffer.size())
        Buffer.resize(Offset + 4096);

    ssize_t len = recv(Fd, &Buffer[Offset], Buffer.size() - Offset, MSG_DONTWAIT);
    if (len > 0)
        Offset += len;
    else if (len == 0)
        return TError(EError::Unknown, "recv return zero");
    else if (errno != EAGAIN && errno != EWOULDBLOCK)
        return TError(EError::Unknown, errno, "recv request failed");

    if (Length && Offset < Length)
        return TError::Queued();

    google::protobuf::io::CodedInputStream input(&Buffer[0], Offset);

    uint32_t length;
    if (!input.ReadVarint32(&length))
        return TError::Queued();

    if (!Length) {
        if (length > config().daemon().max_msg_len())
            return TError(EError::Unknown, "oversized request: " + std::to_string(length));

        Length = length + google::protobuf::io::CodedOutputStream::VarintSize32(length);
        if (Buffer.size() < Length)
            Buffer.resize(Length + 4096);

        if (Offset < Length)
            return TError::Queued();
    }

    if (!request.ParseFromCodedStream(&input))
        return TError(EError::Unknown, "cannot parse request");

    if (Offset > Length)
        return TError(EError::Unknown, "garbage after request");

    Processing = true;
    return EpollLoop->StopInput(Fd);
}

TError TClient::SendResponse(bool first) {
    TScopedLock lock(Mutex);

    if (Fd < 0)
        return TError::Success(); /* Connection closed */

    ssize_t len = send(Fd, &Buffer[Offset], Length - Offset, MSG_DONTWAIT);
    if (len > 0)
        Offset += len;
    else if (len == 0) {
        if (!first)
            return TError(EError::Unknown, "send return zero");
    } else if (errno != EAGAIN && errno != EWOULDBLOCK)
        return TError(EError::Unknown, errno, "send response failed");

    if (Offset >= Length) {
        Length = Offset = 0;
        Processing = false;
        return EpollLoop->StartInput(Fd);
    }

    if (first)
        return EpollLoop->StartOutput(Fd);

    return TError::Success();
}

TError TClient::QueueResponse(rpc::TContainerResponse &response) {
    uint32_t length = response.ByteSize();
    size_t lengthSize = google::protobuf::io::CodedOutputStream::VarintSize32(length);

    Offset = 0;
    Length = lengthSize + length;

    if (Buffer.size() < Length)
        Buffer.resize(Length);

    google::protobuf::io::CodedOutputStream::WriteVarint32ToArray(length, &Buffer[0]);
    if (!response.SerializeToArray(&Buffer[lengthSize], length))
        return TError(EError::Unknown, "cannot serialize response");

    return SendResponse(true);
}

std::ostream& operator<<(std::ostream& stream, TClient& client) {
    if (client.FullLog) {
        client.FullLog = false;
        stream << client.Fd << ":" <<  client.Comm << "(" << client.Pid << ") "
               << client.Cred << " " << client.ClientContainer->Name;
    } else {
        stream << client.Fd << ":" << client.Comm << "(" << client.Pid << ")";
    }
    return stream;
}

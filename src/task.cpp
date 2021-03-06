#include <climits>
#include <sstream>
#include <iterator>
#include <csignal>

#include "task.hpp"
#include "device.hpp"
#include "config.hpp"
#include "util/log.hpp"
#include "util/string.hpp"
#include "util/signal.hpp"
#include "util/unix.hpp"
#include "util/cred.hpp"
#include "util/netlink.hpp"

extern "C" {
#include <string.h>
#include <sys/types.h>
#include <sys/stat.h>
#include <sys/wait.h>
#include <fcntl.h>
#include <unistd.h>
#include <wordexp.h>
#include <grp.h>
#include <net/if.h>
}

void TTaskEnv::ReportPid(pid_t pid) {
    TError error = Sock.SendPid(pid);
    if (error) {
        L_ERR() << error << std::endl;
        Abort(error);
    }
    ReportStage++;
}

void TTaskEnv::Abort(const TError &error) {
    TError error2;

    /*
     * stage0: RecvPid WPid
     * stage1: RecvPid VPid
     * stage2: RecvError
     */
    L() << "abort due to " << error << std::endl;

    for (int stage = ReportStage; stage < 2; stage++) {
        error2 = Sock.SendPid(getpid());
        if (error2)
            L_ERR() << error2 << std::endl;
    }

    error2 = Sock.SendError(error);
    if (error2)
        L_ERR() << error2 << std::endl;

    _exit(EXIT_FAILURE);
}

static int ChildFn(void *arg) {
    SetProcessName("portod-spawn-c");
    TTaskEnv *task = static_cast<TTaskEnv*>(arg);
    task->StartChild();
    return EXIT_FAILURE;
}

TError TTaskEnv::ChildExec() {

    /* set environment for wordexp */
    TError error = Env.Apply();

    auto envp = Env.Envp();

    if (CT->IsMeta()) {
        const char *args[] = {
            "portoinit",
            "--container",
            CT->Name.c_str(),
            NULL,
        };
        SetDieOnParentExit(0);
        TFile::CloseAll({PortoInit.Fd});
        fexecve(PortoInit.Fd, (char *const *)args, envp);
        return TError(EError::InvalidValue, errno, "fexecve(" +
                      std::to_string(PortoInit.Fd) +  ", portoinit)");
    }

    wordexp_t result;

    int ret = wordexp(CT->Command.c_str(), &result, WRDE_NOCMD | WRDE_UNDEF);
    switch (ret) {
    case WRDE_BADCHAR:
        return TError(EError::Unknown, EINVAL, "wordexp(): illegal occurrence of newline or one of |, &, ;, <, >, (, ), {, }");
    case WRDE_BADVAL:
        return TError(EError::Unknown, EINVAL, "wordexp(): undefined shell variable was referenced");
    case WRDE_CMDSUB:
        return TError(EError::Unknown, EINVAL, "wordexp(): command substitution is not supported");
    case WRDE_SYNTAX:
        return TError(EError::Unknown, EINVAL, "wordexp(): syntax error");
    default:
    case WRDE_NOSPACE:
        return TError(EError::Unknown, EINVAL, "wordexp(): error " + std::to_string(ret));
    case 0:
        break;
    }

    if (Verbose) {
        L() << "command=" << CT->Command << std::endl;
        for (unsigned i = 0; result.we_wordv[i]; i++)
            L() << "argv[" << i << "]=" << result.we_wordv[i] << std::endl;
        for (unsigned i = 0; envp[i]; i++)
            L() << "environ[" << i << "]=" << envp[i] << std::endl;
    }
    SetDieOnParentExit(0);
    TFile::CloseAll({0, 1, 2, Sock.GetFd()});
    execvpe(result.we_wordv[0], (char *const *)result.we_wordv, envp);

    return TError(EError::InvalidValue, errno, std::string("execvpe(") +
            result.we_wordv[0] + ", " + std::to_string(result.we_wordc) + ")");
}

TError TTaskEnv::ChildApplyLimits() {
    for (const auto &pair :CT->Rlimit) {
        int ret = setrlimit(pair.first, &pair.second);
        if (ret < 0)
            return TError(EError::Unknown, errno,
                          "setrlimit(" + std::to_string(pair.first) +
                          ", " + std::to_string(pair.second.rlim_cur) +
                          ":" + std::to_string(pair.second.rlim_max) + ")");
    }

    return TError::Success();
}

TError TTaskEnv::WriteResolvConf() {
    std::string cfg;

    if (!CT->ResolvConf.size())
        return TError::Success();

    for (auto &line: CT->ResolvConf)
        cfg += line + "\n";

    return TPath("/etc/resolv.conf").WritePrivate(cfg);
}

TError TTaskEnv::SetHostname() {
    TError error;

    if (CT->Hostname.size()) {
        error = TPath("/etc/hostname").WritePrivate(CT->Hostname + "\n");
        if (!error)
            error = SetHostName(CT->Hostname);
    }

    return error;
}

TError TTaskEnv::ConfigureChild() {
    TError error;

    /* Die together with waiter */
    if (TripleFork)
        SetDieOnParentExit(SIGKILL);

    error = ChildApplyLimits();
    if (error)
        return error;

    if (setsid() < 0)
        return TError(EError::Unknown, errno, "setsid()");

    umask(0);

    if (NewMountNs) {
        // Remount to slave to receive propogations from parent namespace
        error = TPath("/").Remount(MS_SLAVE | MS_REC);
        if (error)
            return error;
    }

    if (CT->Isolate) {
        // remount proc so PID namespace works
        TPath tmpProc("/proc");
        error = tmpProc.UmountAll();
        if (error)
            return error;
        error = tmpProc.Mount("proc", "proc",
                              MS_NOEXEC | MS_NOSUID | MS_NODEV, {});
        if (error)
            return error;
    }

    /* Mount read-only sysfs in new namespaces */
    if (NewMountNs && Mnt.Root.IsRoot()) {
        TPath sys("/sys");
        error = sys.UmountAll();
        if (error)
            return error;
        error = sys.Mount("sysfs", "sysfs", MS_NOSUID | MS_NOEXEC | MS_NODEV | MS_RDONLY, {});
        if (error)
            return error;
    }

    error = Mnt.MountRootFs();
    if (error)
        return error;

    for (auto &dev: Devices) {
        error = dev.Makedev(Mnt.Root);
        if (error)
            return error;
    }

    if (NewMountNs && CT->BindDns && !CT->ResolvConf.size() &&
            !Mnt.Root.IsRoot()) {
        error = Mnt.BindResolvConf();
        if (error)
            return error;
    }

    error =  Mnt.MountBinds();
    if (error)
        return error;

    error =  Mnt.RemountRootRo();
    if (error)
        return error;

    error =  Mnt.IsolateFs();
    if (error)
        return error;

    error = WriteResolvConf();
    if (error)
        return error;

    error = SetHostname();
    if (error)
        return error;

    error = Mnt.Cwd.Chdir();
    if (error)
        return error;

    if (NewMountNs) {
        // Make all shared: subcontainers will get propgation from us
        error = TPath("/").Remount(MS_SHARED | MS_REC);
        if (error)
            return error;
    }

    if (QuadroFork) {
        pid_t pid = fork();
        if (pid < 0)
            return TError(EError::Unknown, errno, "fork()");

        if (pid) {
            auto pid_ = std::to_string(pid);
            const char * argv[] = {
                "portoinit",
                "--container",
                CT->Name.c_str(),
                "--wait",
                pid_.c_str(),
                NULL,
            };
            auto envp = Env.Envp();

            error = PortoInitCapabilities.ApplyLimit();
            if (error)
                return error;

            TFile::CloseAll({PortoInit.Fd});
            fexecve(PortoInit.Fd, (char *const *)argv, envp);
            return TError(EError::Unknown, errno, "fexecve()");
        } else {
            pid = getpid();

            MasterSock2.Close();

            error = Sock2.SendPid(pid);
            if (error)
                return error;
            error = Sock2.RecvZero();
            if (error)
                return error;
            /* Parent forwards VPid */
            ReportStage++;

            Sock2.Close();

            if (setsid() < 0)
                return TError(EError::Unknown, errno, "setsid()");
        }
    }

    error = Cred.Apply();
    if (error)
        return error;

    error = CT->CapAmbient.ApplyAmbient();
    if (error)
        return error;

    error = CT->CapLimit.ApplyLimit();
    if (error)
        return error;

    if (!Cred.IsRootUser()) {
        error = CT->CapAmbient.ApplyEffective();
        if (error)
            return error;
    }

    error = CT->Stdin.OpenInside(*CT);
    if (error)
        return error;

    error = CT->Stdout.OpenInside(*CT);
    if (error)
        return error;

    error = CT->Stderr.OpenInside(*CT);
    if (error)
        return error;

    umask(CT->Umask);

    return TError::Success();
}

TError TTaskEnv::WaitAutoconf() {
    if (Autoconf.empty())
        return TError::Success();

    SetProcessName("portod-autoconf");

    auto sock = std::make_shared<TNl>();
    TError error = sock->Connect();
    if (error)
        return error;

    for (auto &name: Autoconf) {
        TNlLink link(sock, name);

        error = link.Load();
        if (error)
            return error;

        error = link.WaitAddress(config().network().autoconf_timeout_s());
        if (error)
            return error;
    }

    return TError::Success();
}

void TTaskEnv::StartChild() {
    TError error;

    /* WPid reported by parent */
    ReportStage++;

    /* Wait for report WPid in parent */
    error = Sock.RecvZero();
    if (error)
        Abort(error);

    /* Report VPid in pid namespace we're enter */
    if (!CT->Isolate)
        ReportPid(getpid());
    else if (!QuadroFork)
        ReportStage++;

    /* Apply configuration */
    error = ConfigureChild();
    if (error)
        Abort(error);

    /* Wait for Wakeup */
    error = Sock.RecvZero();
    if (error)
        Abort(error);

    /* Reset signals before exec, signal block already lifted */
    ResetIgnoredSignals();

    error = WaitAutoconf();
    if (error)
        Abort(error);

    error = ChildExec();
    Abort(error);
}

TError TTaskEnv::Start() {
    TError error;

    CT->Task.Pid = 0;
    CT->TaskVPid = 0;
    CT->WaitTask.Pid = 0;

    error = TUnixSocket::SocketPair(MasterSock, Sock);
    if (error)
        return error;

    // we want our child to have portod master as parent, so we
    // are doing double fork here (fork + clone);
    // we also need to know child pid so we are using pipe to send it back

    pid_t forkPid = ForkFromThread();
    if (forkPid < 0) {
        Sock.Close();
        TError error(EError::Unknown, errno, "fork()");
        L() << "Can't spawn child: " << error << std::endl;
        return error;
    } else if (forkPid == 0) {
        TError error;

        /* Switch from signafd back to normal signal delivery */
        ResetBlockedSignals();

        SetDieOnParentExit(SIGKILL);

        SetProcessName("portod-spawn-p");

        char stack[8192];

        (void)setsid();

        // move to target cgroups
        for (auto &cg : Cgroups) {
            error = cg.Attach(getpid());
            if (error)
                Abort(error);
        }

        /* Default streams and redirections are outside */
        error = CT->Stdin.OpenOutside(*CT, *Client);
        if (error)
            Abort(error);

        error = CT->Stdout.OpenOutside(*CT, *Client);
        if (error)
            Abort(error);

        error = CT->Stderr.OpenOutside(*CT, *Client);
        if (error)
            Abort(error);

        /* Enter parent namespaces */
        error = ParentNs.Enter();
        if (error)
            Abort(error);

        if (TripleFork) {
            /*
             * Enter into pid-namespace. fork() hangs in libc if child pid
             * collide with parent pid outside. vfork() has no such problem.
             */
            forkPid = vfork();
            if (forkPid < 0)
                Abort(TError(EError::Unknown, errno, "fork()"));

            if (forkPid)
                _exit(EXIT_SUCCESS);
        }

        if (QuadroFork) {
            error = TUnixSocket::SocketPair(MasterSock2, Sock2);
            if (error)
                Abort(error);
        }

        int cloneFlags = SIGCHLD;
        if (CT->Isolate)
            cloneFlags |= CLONE_NEWPID | CLONE_NEWIPC;

        if (NewMountNs)
            cloneFlags |= CLONE_NEWNS;

        /* Create UTS namspace if hostname is changed or isolate=true */
        if (CT->Isolate || CT->Hostname != "")
            cloneFlags |= CLONE_NEWUTS;

        pid_t clonePid = clone(ChildFn, stack + sizeof(stack), cloneFlags, this);

        if (clonePid < 0) {
            TError error(errno == ENOMEM ?
                         EError::ResourceNotAvailable :
                         EError::Unknown, errno, "clone()");
            Abort(error);
        }

        /* Report WPid in host pid namespace */
        if (TripleFork)
            ReportPid(GetTid());
        else
            ReportPid(clonePid);

        /* Report VPid in parent pid namespace for new pid-ns */
        if (CT->Isolate && !QuadroFork)
            ReportPid(clonePid);

        /* WPid reported, wakeup child */
        error = MasterSock.SendZero();
        if (error)
            Abort(error);

        /* ChildCallback() reports VPid here if !Isolate */
        if (!CT->Isolate && !QuadroFork)
            ReportStage++;

        /*
         * QuadroFork waiter receives application VPid from init
         * task and forwards it into host.
         */
        if (QuadroFork) {
            pid_t appPid, appVPid;

            /* close other side before reading */
            Sock2.Close();

            error = MasterSock2.RecvPid(appPid, appVPid);
            if (error)
                Abort(error);
            /* Forward VPid */
            ReportPid(appPid);
            error = MasterSock2.SendZero();
            if (error)
                Abort(error);

            MasterSock2.Close();
        }

        if (TripleFork) {
            auto pid = std::to_string(clonePid);
            const char * argv[] = {
                "portoinit",
                "--container",
                CT->Name.c_str(),
                "--wait",
                pid.c_str(),
                NULL,
            };
            auto envp = Env.Envp();

            error = PortoInitCapabilities.ApplyLimit();
            if (error)
                _exit(EXIT_FAILURE);

            TFile::CloseAll({PortoInit.Fd});
            fexecve(PortoInit.Fd, (char *const *)argv, envp);
            kill(clonePid, SIGKILL);
            _exit(EXIT_FAILURE);
        }

        _exit(EXIT_SUCCESS);
    }

    Sock.Close();

    error = MasterSock.SetRecvTimeout(config().container().start_timeout_ms());
    if (error)
        goto kill_all;

    error = MasterSock.RecvPid(CT->WaitTask.Pid, CT->TaskVPid);
    if (error)
        goto kill_all;

    error = MasterSock.RecvPid(CT->Task.Pid, CT->TaskVPid);
    if (error)
        goto kill_all;

    int status;
    if (waitpid(forkPid, &status, 0) < 0) {
        error = TError(EError::Unknown, errno, "wait for middle task failed");
        goto kill_all;
    }
    forkPid = 0;

    /* Task was alive, even if it already died we'll get zombie */
    error = MasterSock.SendZero();
    if (error)
        L() << "Task wakeup error: " << error << std::endl;

    /* Prefer reported error if any */
    error = MasterSock.RecvError();
    if (error)
        goto kill_all;

    if (!error && status) {
        error = TError(EError::Unknown, "Start failed, status " + std::to_string(status));
        goto kill_all;
    }

    return TError::Success();

kill_all:
    L_ACT() << "Kill partialy constructed container: " << error << std::endl;
    for (auto &cg : Cgroups)
        (void)cg.KillAll(SIGKILL);
    if (forkPid) {
        (void)kill(forkPid, SIGKILL);
        (void)waitpid(forkPid, nullptr, 0);
    }
    CT->Task.Pid = 0;
    CT->TaskVPid = 0;
    CT->WaitTask.Pid = 0;
    return error;
}

// Functions for executing the jobs builtin.
#include "config.h"  // IWYU pragma: keep

#include <errno.h>
#include <stdbool.h>
#ifdef HAVE__PROC_SELF_STAT
#include <sys/time.h>
#endif

#include "builtin.h"
#include "common.h"
#include "docopt_registration.h"
#include "fallback.h"  // IWYU pragma: keep
#include "io.h"
#include "proc.h"
#include "wgetopt.h"
#include "wutil.h"  // IWYU pragma: keep

class parser_t;

/// Print modes for the jobs builtin.
enum jobs_mode_t {
    JOBS_DEFAULT,        // print lots of general info
    JOBS_PRINT_PID,      // print pid of each process in job
    JOBS_PRINT_COMMAND,  // print command name of each process in job
    JOBS_PRINT_GROUP,    // print group id of job
};

#ifdef HAVE__PROC_SELF_STAT
/// Calculates the cpu usage (in percent) of the specified job.
static int cpu_use(const job_t *j) {
    double u = 0;
    process_t *p;

    for (p = j->first_process; p; p = p->next) {
        struct timeval t;
        int jiffies;
        gettimeofday(&t, 0);
        jiffies = proc_get_jiffies(p);

        double t1 = 1000000.0 * p->last_time.tv_sec + p->last_time.tv_usec;
        double t2 = 1000000.0 * t.tv_sec + t.tv_usec;

        // fwprintf( stderr, L"t1 %f t2 %f p1 %d p2 %d\n", t1, t2, jiffies, p->last_jiffies );
        u += ((double)(jiffies - p->last_jiffies)) / (t2 - t1);
    }
    return u * 1000000;
}
#endif

/// Print information about the specified job.
static void builtin_jobs_print(const job_t *j, int mode, int header, io_streams_t &streams) {
    process_t *p;
    switch (mode) {
        case JOBS_DEFAULT: {
            if (header) {
                // Print table header before first job.
                streams.out.append(_(L"Job\tGroup\t"));
#ifdef HAVE__PROC_SELF_STAT
                streams.out.append(_(L"CPU\t"));
#endif
                streams.out.append(_(L"State\tCommand\n"));
            }

            streams.out.append_format(L"%d\t%d\t", j->job_id, j->pgid);

#ifdef HAVE__PROC_SELF_STAT
            streams.out.append_format(L"%d%%\t", cpu_use(j));
#endif
            streams.out.append(job_is_stopped(j) ? _(L"stopped") : _(L"running"));
            streams.out.append(L"\t");
            streams.out.append(j->command_wcstr());
            streams.out.append(L"\n");
            break;
        }
        case JOBS_PRINT_GROUP: {
            if (header) {
                // Print table header before first job.
                streams.out.append(_(L"Group\n"));
            }
            streams.out.append_format(L"%d\n", j->pgid);
            break;
        }
        case JOBS_PRINT_PID: {
            if (header) {
                // Print table header before first job.
                streams.out.append(_(L"Process\n"));
            }

            for (p = j->first_process; p; p = p->next) {
                streams.out.append_format(L"%d\n", p->pid);
            }
            break;
        }
        case JOBS_PRINT_COMMAND: {
            if (header) {
                // Print table header before first job.
                streams.out.append(_(L"Command\n"));
            }

            for (p = j->first_process; p; p = p->next) {
                streams.out.append_format(L"%ls\n", p->argv0());
            }
            break;
        }
    }
}

extern const wchar_t *const g_jobs_usage =
    L"Usage:\n"
    L"       jobs [options] [<pid>...]\n"
    L"\n"
    L"Options:\n"
    L"       -c, --command  prints the command name for each process in jobs.\n"
    L"       -g, --group  only prints the group ID of each job.\n"
    L"       -h, --help  displays a help message and exits.\n"
    L"       -l, --last  prints only the last job to be started.\n"
    L"       -p, --pid  prints the process ID for each process in all jobs.\n"
    L"Conditions:\n"
    L"       <pid>  (jobs --pid)";

/** The jobs builtin. Used for printing running jobs. */
int builtin_jobs(parser_t &parser, io_streams_t &streams, wchar_t **argv) {
    docopt_arguments_t args;
    int status;
    if (!parse_argv_or_show_help(parser, argv, &args, &status, streams)) {
        return status;
    }

    int found = 0;
    jobs_mode_t mode = JOBS_DEFAULT;
    if (args.has(L"--pid")) {
        mode = JOBS_PRINT_PID;
    } else if (args.has(L"--command")) {
        mode = JOBS_PRINT_COMMAND;
    } else if (args.has(L"--group")) {
        mode = JOBS_PRINT_GROUP;
    }

    if (args.has(L"--last")) {
        /* Ignore unconstructed jobs, i.e. ourself. */
        job_iterator_t jobs;
        const job_t *j;
        while ((j = jobs.next())) {
            if ((j->flags & JOB_CONSTRUCTED) && !job_is_completed(j)) {
                builtin_jobs_print(j, mode, !found && !streams.out_is_redirected, streams);
                return 0;
            }
        }
    } else {
        const wcstring_list_t &pids = args.get_list(L"<pid>");
        if (!pids.empty()) {
            for (size_t i = 0; i < pids.size(); i++) {
                int pid;
                wchar_t *end;
                errno = 0;
                pid = fish_wcstoi(pids.at(i).c_str(), &end, 10);
                if (errno || *end) {
                    streams.err.append_format(_(L"%ls: '%ls' is not a job\n"), argv[0], argv[i]);
                    return 1;
                }

                const job_t *j = job_get_from_pid(pid);

                if (j && !job_is_completed(j)) {
                    builtin_jobs_print(j, mode, false, streams);
                    found = 1;
                } else {
                    streams.err.append_format(_(L"%ls: No suitable job: %d\n"), argv[0], pid);
                    return 1;
                }
            }
        } else {
            job_iterator_t jobs;
            const job_t *j;
            while ((j = jobs.next())) {
                // Ignore unconstructed jobs, i.e. ourself.
                if ((j->flags & JOB_CONSTRUCTED) && !job_is_completed(j)) {
                    builtin_jobs_print(j, mode, !streams.out_is_redirected, streams);
                    found = 1;
                }
            }
        }
    }

    if (!found) {
        // Do not babble if not interactive.
        if (!streams.out_is_redirected) {
            streams.out.append_format(_(L"%ls: There are no jobs\n"), argv[0]);
        }
        return 1;
    }

    return status;
}

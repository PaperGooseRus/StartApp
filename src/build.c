#ifndef _WIN32
#define _GNU_SOURCE
#define _POSIX_C_SOURCE 200809L
#endif
#include "build.h"
#include <string.h>
#ifdef G_OS_WIN32
#include <windows.h>
#else
#include <signal.h>
#include <unistd.h>
#include <fcntl.h>
#include <errno.h>
#include <spawn.h>
#include <sys/wait.h>
#endif

static gboolean option_value(const char *text)
{ return text && *text && *text != '-' && strlen(text) <= 1024 && !strchr(text, '\n') && !strchr(text, '\r'); }

BuildPlan *build_plan_new(const Project *project, gint target_index, const char *cmake, GError **error)
{
    if (!project || !project->build_directory || !project->generator || !cmake || !*cmake) {
        g_set_error_literal(error, G_IO_ERROR, G_IO_ERROR_NOT_SUPPORTED, "Нужны настроенный CMake-проект и доступный cmake."); return NULL;
    }
    g_autofree char *cache = g_build_filename(project->build_directory, "CMakeCache.txt", NULL);
    if (!g_file_test(cache, G_FILE_TEST_IS_REGULAR)) {
        g_set_error_literal(error, G_IO_ERROR, G_IO_ERROR_NOT_FOUND, "В каталоге сборки отсутствует CMakeCache.txt. Сначала выполните конфигурацию проекта."); return NULL;
    }
    const char *target = NULL;
    if (target_index >= 0) {
        if ((guint)target_index >= project->targets->len) {
            g_set_error_literal(error, G_IO_ERROR, G_IO_ERROR_INVALID_ARGUMENT, "Цель сборки не найдена."); return NULL;
        }
        target = ((ProjectTarget *)g_ptr_array_index(project->targets, (guint)target_index))->name;
    }
    if ((target && !option_value(target)) || (project->configuration && *project->configuration && !option_value(project->configuration))) {
        g_set_error_literal(error, G_IO_ERROR, G_IO_ERROR_INVALID_ARGUMENT, "Неподдерживаемое имя цели или конфигурации."); return NULL;
    }
    GPtrArray *args = g_ptr_array_new_with_free_func(g_free);
    g_ptr_array_add(args, g_strdup(cmake)); g_ptr_array_add(args, g_strdup("--build"));
    g_ptr_array_add(args, g_strdup(project->build_directory));
    if (project->configuration && *project->configuration) {
        g_ptr_array_add(args, g_strdup("--config")); g_ptr_array_add(args, g_strdup(project->configuration));
    }
    if (target) { g_ptr_array_add(args, g_strdup("--target")); g_ptr_array_add(args, g_strdup(target)); }
    g_ptr_array_add(args, g_strdup("--parallel")); g_ptr_array_add(args, g_strdup("2"));
    GString *display = g_string_new(NULL);
    for (guint i = 0; i < args->len; i++) {
        g_autofree char *quoted = g_shell_quote(g_ptr_array_index(args, i));
        if (i) g_string_append_c(display, ' ');
        g_string_append(display, quoted);
    }
    BuildPlan *plan = g_new0(BuildPlan, 1); plan->display = g_string_free(display, FALSE);
    plan->directory = g_strdup(project->build_directory); g_ptr_array_add(args, NULL);
    plan->argv = (char **)g_ptr_array_free(args, FALSE); return plan;
}
void build_plan_free(BuildPlan *plan)
{ if (plan) { g_strfreev(plan->argv); g_free(plan->directory); g_free(plan->display); g_free(plan); } }

BuildDiagnostic *build_diagnostic_parse(const char *text, const char *directory, const Project *project)
{
    g_autoptr(GRegex) ansi = g_regex_new("\033\\[[0-9;]*[A-Za-z]", 0, 0, NULL);
    g_autofree char *clean = g_regex_replace_literal(ansi, text, -1, 0, "", 0, NULL);
    const char *patterns[] = {"^(.+?):([0-9]+)(?::([0-9]+))?:\\s*(.+)$", "^(.+?)\\(([0-9]+)(?:,([0-9]+))?\\)\\s*:\\s*(.+)$"};
    for (guint i = 0; i < G_N_ELEMENTS(patterns); i++) {
        g_autoptr(GRegex) regex = g_regex_new(patterns[i], 0, 0, NULL);
        g_autoptr(GMatchInfo) match = NULL;
        if (!g_regex_match(regex, clean, 0, &match)) continue;
        g_autofree char *file = g_match_info_fetch(match, 1), *line = g_match_info_fetch(match, 2);
        g_autofree char *column = g_match_info_fetch(match, 3), *message = g_match_info_fetch(match, 4);
        guint64 row = g_ascii_strtoull(line, NULL, 10), col = g_ascii_strtoull(column, NULL, 10);
        if (!row || row > G_MAXUINT || col > G_MAXUINT) return NULL;
        g_strstrip(file);
        if (file[0] == '"' && strlen(file) > 1 && file[strlen(file)-1] == '"') { file[strlen(file)-1] = 0; memmove(file, file+1, strlen(file)); }
        BuildDiagnostic *diagnostic = g_new0(BuildDiagnostic, 1);
        diagnostic->file = g_strdup(file); diagnostic->line = (guint)row; diagnostic->column = (guint)col;
        diagnostic->message = g_strdup(message);
        g_autofree char *folded = g_utf8_casefold(message, -1);
        diagnostic->warning = strstr(folded, "warning") || strstr(folded, "предупреждение");
        if (project) {
            g_autofree char *absolute = g_canonicalize_filename(file, directory);
            gint index = project_find_absolute(project, absolute);
            if (index < 0 && !g_path_is_absolute(file)) {
                g_free(absolute); absolute = g_canonicalize_filename(file, project->root); index = project_find_absolute(project, absolute);
            }
            if (index >= 0) diagnostic->project_path = g_strdup(((ProjectFile *)g_ptr_array_index(project->files, (guint)index))->path);
        }
        return diagnostic;
    }
    return NULL;
}
void build_diagnostic_free(gpointer data)
{
    BuildDiagnostic *diagnostic = data;
    if (diagnostic) { g_free(diagnostic->file); g_free(diagnostic->project_path); g_free(diagnostic->message); g_free(diagnostic); }
}

struct BuildRun {
    gint refs, stopped;
    GMutex mutex;
    GMainContext *context;
    char **argv;
    char *directory;
    GQueue lines;
    gsize queued_bytes;
    gboolean scheduled, complete, delivered, active, truncated;
    gint exit_code;
    BuildOutput output;
    BuildFinished finished;
    gpointer data;
#ifdef G_OS_WIN32
    HANDLE job;
#else
    pid_t group;
#endif
};
static BuildRun *run_ref(BuildRun *run) { g_atomic_int_inc(&run->refs); return run; }
void build_run_unref(BuildRun *run)
{
    if (!run || !g_atomic_int_dec_and_test(&run->refs)) return;
    g_queue_clear_full(&run->lines, g_free); g_strfreev(run->argv); g_free(run->directory);
    g_main_context_unref(run->context); g_mutex_clear(&run->mutex); g_free(run);
}
gboolean build_run_active(BuildRun *run)
{ if (!run) return FALSE; g_mutex_lock(&run->mutex); gboolean active = !run->delivered; g_mutex_unlock(&run->mutex); return active; }
void build_run_stop(BuildRun *run)
{
    if (!run) return;
    g_atomic_int_set(&run->stopped, TRUE);
    g_mutex_lock(&run->mutex);
#ifdef G_OS_WIN32
    if (run->job) TerminateJobObject(run->job, 1);
#else
    if (run->group > 0) kill(-run->group, SIGKILL);
#endif
    g_mutex_unlock(&run->mutex);
}

static gboolean deliver(gpointer data)
{
    BuildRun *run = data;
    for (guint i = 0; i < 128; i++) {
        g_mutex_lock(&run->mutex);
        char *line = g_queue_pop_head(&run->lines);
        if (line) run->queued_bytes -= strlen(line);
        gboolean complete = run->complete && !line;
        if (complete) run->delivered = TRUE;
        gboolean empty = !line;
        if (empty) run->scheduled = FALSE;
        gboolean truncated = empty && run->truncated;
        if (truncated) run->truncated = FALSE;
        g_mutex_unlock(&run->mutex);
        if (line) { if (run->output) run->output(line, run->data); g_free(line); }
        else {
            if (truncated && run->output) run->output("… часть слишком быстрого вывода пропущена …", run->data);
            if (complete && run->finished) run->finished(run->exit_code, g_atomic_int_get(&run->stopped), run->data);
            return G_SOURCE_REMOVE;
        }
    }
    return G_SOURCE_CONTINUE;
}
static void schedule_locked(BuildRun *run)
{
    if (run->scheduled) return;
    run->scheduled = TRUE;
    GSource *source = g_idle_source_new();
    g_source_set_callback(source, deliver, run_ref(run), (GDestroyNotify)build_run_unref);
    g_source_attach(source, run->context); g_source_unref(source);
}
static void enqueue(BuildRun *run, const char *bytes, gssize length)
{
    char *line = g_utf8_make_valid(bytes, length);
#ifdef G_OS_WIN32
    if (!g_utf8_validate(bytes, length, NULL)) {
        gint count = length < 0 ? (gint)strlen(bytes) : (gint)length;
        gint needed = MultiByteToWideChar(CP_ACP, 0, bytes, count, NULL, 0);
        if (needed > 0) {
            g_autofree gunichar2 *wide = g_new0(gunichar2, needed + 1);
            if (MultiByteToWideChar(CP_ACP, 0, bytes, count, (LPWSTR)wide, needed)) {
                char *converted = g_utf16_to_utf8(wide, needed, NULL, NULL, NULL);
                if (converted) { g_free(line); line = converted; }
            }
        }
    }
#endif
    g_mutex_lock(&run->mutex);
    if (run->lines.length >= 256 || run->queued_bytes + strlen(line) > 256 * 1024) { run->truncated = TRUE; g_free(line); }
    else { run->queued_bytes += strlen(line); g_queue_push_tail(&run->lines, line); }
    schedule_locked(run); g_mutex_unlock(&run->mutex);
}
static void feed(BuildRun *run, GString *pending, const char *bytes, gsize length)
{
    for (gsize i = 0; i < length; i++) {
        if (bytes[i] == '\n' || bytes[i] == '\r') {
            if (pending->len) { enqueue(run, pending->str, (gssize)pending->len); g_string_truncate(pending, 0); }
        } else {
            g_string_append_c(pending, bytes[i] ? bytes[i] : '?');
            if (pending->len >= 64 * 1024) { enqueue(run, pending->str, (gssize)pending->len); g_string_truncate(pending, 0); }
        }
    }
}

#ifdef G_OS_WIN32
/* Create suspended, attach the job, then resume: children cannot escape by racing assignment. */
static char *windows_command(char **args)
{
    GString *command = g_string_new(NULL);
    for (guint i = 0; args[i]; i++) {
        if (i) g_string_append_c(command, ' ');
        g_string_append_c(command, '"'); const char *p = args[i];
        while (*p) {
            guint slashes = 0; while (*p == '\\') { slashes++; p++; }
            guint copies = *p == '"' || !*p ? slashes * 2 : slashes;
            for (guint j = 0; j < copies; j++) g_string_append_c(command, '\\');
            if (*p == '"') g_string_append_c(command, '\\');
            if (*p) g_string_append_c(command, *p++);
        }
        g_string_append_c(command, '"');
    }
    return g_string_free(command, FALSE);
}
static gint execute(BuildRun *run)
{
    HANDLE read_pipe = NULL, write_pipe = NULL, job = NULL, input = INVALID_HANDLE_VALUE;
    SECURITY_ATTRIBUTES security = {sizeof(security), NULL, TRUE};
    PROCESS_INFORMATION process = {0}; STARTUPINFOW startup = {0}; startup.cb = sizeof(startup);
    g_autofree char *exe = g_find_program_in_path(run->argv[0]);
    if (!exe) {
        g_autofree char *message = g_strdup_printf("Не найден инструмент сборки: %s", run->argv[0]);
        enqueue(run, message, -1); return -1;
    }
    g_autofree char *command = windows_command(run->argv);
    g_autofree gunichar2 *wide_exe = exe ? g_utf8_to_utf16(exe, -1, NULL, NULL, NULL) : NULL;
    g_autofree gunichar2 *wide_command = g_utf8_to_utf16(command, -1, NULL, NULL, NULL);
    g_autofree gunichar2 *wide_directory = g_utf8_to_utf16(run->directory, -1, NULL, NULL, NULL);
    if (!wide_exe || !wide_command || !wide_directory || !CreatePipe(&read_pipe, &write_pipe, &security, 0) ||
        !SetHandleInformation(read_pipe, HANDLE_FLAG_INHERIT, 0)) goto failed;
    input = CreateFileW(L"NUL", GENERIC_READ, FILE_SHARE_READ | FILE_SHARE_WRITE, &security, OPEN_EXISTING, 0, NULL);
    if (input == INVALID_HANDLE_VALUE) goto failed;
    job = CreateJobObjectW(NULL, NULL);
    JOBOBJECT_EXTENDED_LIMIT_INFORMATION limits = {0}; limits.BasicLimitInformation.LimitFlags = JOB_OBJECT_LIMIT_KILL_ON_JOB_CLOSE;
    if (!job || !SetInformationJobObject(job, JobObjectExtendedLimitInformation, &limits, sizeof(limits))) goto failed;
    startup.dwFlags = STARTF_USESTDHANDLES; startup.hStdInput = input; startup.hStdOutput = startup.hStdError = write_pipe;
    g_mutex_lock(&run->mutex);
    gboolean created = !g_atomic_int_get(&run->stopped) && CreateProcessW((LPCWSTR)wide_exe, (LPWSTR)wide_command, NULL, NULL, TRUE,
        CREATE_SUSPENDED | CREATE_NO_WINDOW, NULL, (LPCWSTR)wide_directory, &startup, &process);
    if (created && !AssignProcessToJobObject(job, process.hProcess)) { TerminateProcess(process.hProcess, 1); created = FALSE; }
    if (created) {
        run->job = job; run->active = TRUE;
        if (ResumeThread(process.hThread) == (DWORD)-1) { TerminateJobObject(job, 1); created = FALSE; }
    }
    g_mutex_unlock(&run->mutex);
    if (!created) goto failed;
    CloseHandle(write_pipe); write_pipe = NULL; CloseHandle(input); input = INVALID_HANDLE_VALUE;
    GString *pending = g_string_new(NULL); char bytes[4096]; DWORD read_count;
    while (ReadFile(read_pipe, bytes, sizeof(bytes), &read_count, NULL) && read_count) feed(run, pending, bytes, read_count);
    if (pending->len) enqueue(run, pending->str, (gssize)pending->len);
    g_string_free(pending, TRUE);
    WaitForSingleObject(process.hProcess, INFINITE);
    DWORD status = 1; GetExitCodeProcess(process.hProcess, &status);
    g_mutex_lock(&run->mutex); run->job = NULL; run->active = FALSE; CloseHandle(job); job = NULL; g_mutex_unlock(&run->mutex);
    CloseHandle(process.hThread); CloseHandle(process.hProcess); CloseHandle(read_pipe);
    return status <= G_MAXINT ? (gint)status : -1;
failed:
    if (!g_atomic_int_get(&run->stopped)) {
        g_autofree char *message = g_strdup_printf("Не удалось запустить инструмент сборки (Windows error %lu).", (unsigned long)GetLastError());
        enqueue(run, message, -1);
    }
    g_mutex_lock(&run->mutex); run->job = NULL; run->active = FALSE; if (job) CloseHandle(job); g_mutex_unlock(&run->mutex);
    if (process.hThread) CloseHandle(process.hThread);
    if (process.hProcess) { WaitForSingleObject(process.hProcess, INFINITE); CloseHandle(process.hProcess); }
    if (read_pipe) CloseHandle(read_pipe);
    if (write_pipe) CloseHandle(write_pipe);
    if (input != INVALID_HANDLE_VALUE) CloseHandle(input);
    return -1;
}
#else
static gint execute(BuildRun *run)
{
    int descriptors[2];
    if (pipe2(descriptors, O_CLOEXEC) != 0) { enqueue(run, g_strerror(errno), -1); return -1; }
    for (guint i = 0; i < 2; i++) if (descriptors[i] <= STDERR_FILENO) {
        int duplicate = fcntl(descriptors[i], F_DUPFD_CLOEXEC, STDERR_FILENO + 1);
        if (duplicate < 0) { enqueue(run, g_strerror(errno), -1); close(descriptors[0]); close(descriptors[1]); return -1; }
        close(descriptors[i]); descriptors[i] = duplicate;
    }
    posix_spawn_file_actions_t actions; posix_spawnattr_t attributes;
    posix_spawn_file_actions_init(&actions); posix_spawnattr_init(&attributes);
    posix_spawn_file_actions_addopen(&actions, STDIN_FILENO, "/dev/null", O_RDONLY, 0);
    posix_spawn_file_actions_adddup2(&actions, descriptors[1], STDOUT_FILENO);
    posix_spawn_file_actions_adddup2(&actions, descriptors[1], STDERR_FILENO);
    posix_spawn_file_actions_addclose(&actions, descriptors[0]);
    posix_spawn_file_actions_addclose(&actions, descriptors[1]);
    posix_spawn_file_actions_addchdir_np(&actions, run->directory);
    posix_spawnattr_setflags(&attributes, POSIX_SPAWN_SETPGROUP);
    posix_spawnattr_setpgroup(&attributes, 0);
    g_auto(GStrv) environment = g_get_environ(); environment = g_environ_setenv(environment, "NO_COLOR", "1", TRUE);
    pid_t process = 0;
    g_mutex_lock(&run->mutex);
    int error = g_atomic_int_get(&run->stopped) ? ECANCELED : posix_spawnp(&process, run->argv[0], &actions, &attributes, run->argv, environment);
    if (!error) { run->group = process; run->active = TRUE; }
    g_mutex_unlock(&run->mutex);
    posix_spawn_file_actions_destroy(&actions); posix_spawnattr_destroy(&attributes); close(descriptors[1]);
    if (error) { close(descriptors[0]); if (error != ECANCELED) enqueue(run, g_strerror(error), -1); return -1; }
    GString *pending = g_string_new(NULL); char bytes[4096]; gssize count;
    while (TRUE) {
        count = read(descriptors[0], bytes, sizeof(bytes));
        if (count > 0) feed(run, pending, bytes, (gsize)count);
        else if (count < 0 && errno == EINTR) continue;
        else break;
    }
    if (pending->len) enqueue(run, pending->str, (gssize)pending->len);
    g_string_free(pending, TRUE); close(descriptors[0]);
    /* Retain the leader's PID until its entire group has been cleaned up. */
    siginfo_t info; int waited;
    do { waited = waitid(P_PID, process, &info, WEXITED | WNOWAIT); } while (waited < 0 && errno == EINTR);
    g_mutex_lock(&run->mutex);
    if (run->group > 0) kill(-run->group, SIGKILL);
    run->group = 0; run->active = FALSE; g_mutex_unlock(&run->mutex);
    int status = 0; pid_t reaped;
    do { reaped = waitpid(process, &status, 0); } while (reaped < 0 && errno == EINTR);
    return waited == 0 && reaped == process && WIFEXITED(status) ? WEXITSTATUS(status) : -1;
}
#endif
static void worker(GTask *task, gpointer source, gpointer data, GCancellable *cancel)
{
    (void)source; (void)cancel; BuildRun *run = data;
    gint code = execute(run);
    g_mutex_lock(&run->mutex); run->exit_code = code; run->complete = TRUE; schedule_locked(run); g_mutex_unlock(&run->mutex);
    g_task_return_boolean(task, TRUE);
}
BuildRun *build_run_start(const char *const *argv, const char *directory, BuildOutput output, BuildFinished finished, gpointer data)
{
    g_return_val_if_fail(argv && argv[0] && directory, NULL);
    BuildRun *run = g_new0(BuildRun, 1); run->refs = 1; g_mutex_init(&run->mutex);
    run->context = g_main_context_ref_thread_default(); run->argv = g_strdupv((char **)argv); run->directory = g_strdup(directory);
    run->output = output; run->finished = finished; run->data = data;
    GTask *task = g_task_new(NULL, NULL, NULL, NULL);
    g_task_set_task_data(task, run_ref(run), (GDestroyNotify)build_run_unref);
    g_task_run_in_thread(task, worker); g_object_unref(task); return run;
}

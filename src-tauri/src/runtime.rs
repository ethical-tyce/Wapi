use std::{
    collections::HashSet,
    env,
    io::Read,
    path::{Path, PathBuf},
    process::{Command, Stdio},
    sync::Mutex,
    thread,
    time::{Duration, Instant},
};

use base64::{engine::general_purpose::STANDARD, Engine as _};
use tauri::State;
use wait_timeout::ChildExt;

use crate::models::{ExecuteOptions, ExecutePayload, ExecuteResult, ShellPayload, ShellResult};

const SHELL_CWD_MARKER: &str = "__WAPI_CWD__";
const OUTPUT_LIMIT_MESSAGE: &str =
    "\n[WAPI_IDE] Output limit reached. Terminating Wapi process to protect memory.";

#[derive(Default)]
pub struct RunningCommands(pub Mutex<HashSet<String>>);

fn push_candidate(candidates: &mut Vec<PathBuf>, candidate: PathBuf) {
    if !candidates.iter().any(|existing| existing.eq(&candidate)) {
        candidates.push(candidate);
    }
}

fn candidate_executables() -> Vec<PathBuf> {
    let mut candidates = Vec::new();
    if let Ok(explicit) = env::var("WAPI_EXE") {
        if !explicit.trim().is_empty() {
            push_candidate(&mut candidates, PathBuf::from(explicit));
        }
    }

    let mut roots = Vec::new();
    if let Ok(current) = env::current_dir() {
        roots.push(current);
    }
    let manifest_root = Path::new(env!("CARGO_MANIFEST_DIR"))
        .parent()
        .unwrap_or_else(|| Path::new(env!("CARGO_MANIFEST_DIR")))
        .to_path_buf();
    roots.push(manifest_root);

    if let Ok(executable) = env::current_exe() {
        if let Some(parent) = executable.parent() {
            roots.push(parent.to_path_buf());
            if let Some(grandparent) = parent.parent() {
                roots.push(grandparent.to_path_buf());
            }
        }
    }

    for root in roots {
        for relative in [
            PathBuf::from("x64/Debug/Wapi.exe"),
            PathBuf::from("x64/Release/Wapi.exe"),
            PathBuf::from("ARM64/Debug/Wapi.exe"),
            PathBuf::from("ARM64/Release/Wapi.exe"),
            PathBuf::from("Wapi.exe"),
        ] {
            push_candidate(&mut candidates, root.join(relative));
        }
    }
    candidates
}

fn resolve_wapi_executable_inner() -> Option<PathBuf> {
    candidate_executables()
        .into_iter()
        .find(|candidate| candidate.is_file())
}

#[tauri::command]
pub fn locate() -> Option<String> {
    resolve_wapi_executable_inner().map(|path| path.to_string_lossy().into_owned())
}

fn build_args(command: &str, source: &str, options: &ExecuteOptions) -> Vec<String> {
    let mode = match options.mode.as_deref() {
        Some("dev") => "dev",
        Some("unsafe") => "unsafe",
        _ => "safe",
    };
    let mut args = vec![
        command.to_string(),
        source.to_string(),
        "--mode".into(),
        mode.into(),
    ];
    if options.allow_injection {
        args.push("--allow-injection".into());
    }
    if options.strict_permissions {
        args.push("--strict-permissions".into());
    }
    for capability in &options.capabilities {
        let capability = capability.trim();
        if !capability.is_empty() {
            args.push("--cap".into());
            args.push(capability.into());
        }
    }
    args
}

fn read_limited<R: Read + Send + 'static>(
    mut pipe: R,
    limit: usize,
) -> thread::JoinHandle<(Vec<u8>, bool)> {
    thread::spawn(move || {
        let mut bytes = Vec::new();
        let mut buffer = [0_u8; 8192];
        let mut truncated = false;
        loop {
            match pipe.read(&mut buffer) {
                Ok(0) => break,
                Ok(count) => {
                    let remaining = limit.saturating_sub(bytes.len());
                    if remaining == 0 {
                        truncated = true;
                        continue;
                    }
                    let accepted = remaining.min(count);
                    bytes.extend_from_slice(&buffer[..accepted]);
                    if accepted < count {
                        truncated = true;
                    }
                }
                Err(_) => break,
            }
        }
        (bytes, truncated)
    })
}

fn run_process(
    executable: &Path,
    args: Vec<String>,
    timeout: Duration,
    max_output_bytes: usize,
) -> ExecuteResult {
    let started = Instant::now();
    let mut child = match Command::new(executable)
        .args(args)
        .current_dir(
            executable
                .parent()
                .filter(|parent| !parent.as_os_str().is_empty())
                .unwrap_or_else(|| Path::new(".")),
        )
        .stdin(Stdio::null())
        .stdout(Stdio::piped())
        .stderr(Stdio::piped())
        .creation_flags(0x0800_0000)
        .spawn()
    {
        Ok(child) => child,
        Err(error) => {
            let mut result = ExecuteResult::failure(error.to_string());
            result.exe = Some(executable.to_string_lossy().into_owned());
            result.duration_ms = started.elapsed().as_millis();
            return result;
        }
    };

    let pid = child.id();
    let output_limit = max_output_bytes / 2;
    let stdout_reader = child
        .stdout
        .take()
        .map(|pipe| read_limited(pipe, output_limit));
    let stderr_reader = child
        .stderr
        .take()
        .map(|pipe| read_limited(pipe, output_limit));

    let mut timed_out = false;
    let status = match child.wait_timeout(timeout) {
        Ok(Some(status)) => Some(status),
        Ok(None) => {
            timed_out = true;
            let _ = child.kill();
            child.wait().ok()
        }
        Err(_) => {
            let _ = child.kill();
            child.wait().ok()
        }
    };

    let (stdout_bytes, stdout_truncated) = stdout_reader
        .and_then(|reader| reader.join().ok())
        .unwrap_or_default();
    let (stderr_bytes, stderr_truncated) = stderr_reader
        .and_then(|reader| reader.join().ok())
        .unwrap_or_default();
    let output_truncated = stdout_truncated || stderr_truncated;

    let stdout = String::from_utf8_lossy(&stdout_bytes).into_owned();
    let mut stderr = String::from_utf8_lossy(&stderr_bytes).into_owned();
    if timed_out {
        stderr.push_str(&format!(
            "\n[WAPI_IDE] Timed out after {} seconds.",
            timeout.as_secs()
        ));
    }
    if output_truncated {
        stderr.push_str(OUTPUT_LIMIT_MESSAGE);
    }

    let code = status.and_then(|status| status.code());
    ExecuteResult {
        ok: code == Some(0) && !timed_out && !output_truncated,
        code,
        stdout,
        stderr,
        exe: Some(executable.to_string_lossy().into_owned()),
        pid: Some(pid),
        duration_ms: started.elapsed().as_millis(),
        timed_out,
        output_truncated,
    }
}

#[tauri::command]
pub async fn execute(
    payload: ExecutePayload,
    running: State<'_, RunningCommands>,
) -> Result<ExecuteResult, String> {
    let command = if payload.command == "run" {
        "run".to_string()
    } else {
        "check".to_string()
    };

    {
        let mut commands = running
            .0
            .lock()
            .map_err(|_| "Runtime command state is unavailable.".to_string())?;
        if !commands.insert(command.clone()) {
            return Ok(ExecuteResult::failure(format!(
                "A Wapi {command} command is already running."
            )));
        }
    }

    let executable = resolve_wapi_executable_inner();
    let command_for_task = command.clone();
    let result = match executable {
        None => ExecuteResult::failure(
            "Wapi.exe was not found. Build the C++ project or set WAPI_EXE to the executable path.",
        ),
        Some(executable) => {
            let timeout = if command == "check" {
                Duration::from_secs(8)
            } else {
                Duration::from_secs(60)
            };
            let max_output = if command == "check" {
                256 * 1024
            } else {
                1024 * 1024
            };
            let args = build_args(&command, &payload.source, &payload.options);
            tauri::async_runtime::spawn_blocking(move || {
                run_process(&executable, args, timeout, max_output)
            })
            .await
            .unwrap_or_else(|error| {
                ExecuteResult::failure(format!("Wapi {command_for_task} failed: {error}"))
            })
        }
    };

    if let Ok(mut commands) = running.0.lock() {
        commands.remove(&command);
    }
    Ok(result)
}

fn resolve_shell_cwd(requested: Option<String>) -> PathBuf {
    if let Some(path) = requested {
        let path = PathBuf::from(path);
        if path.is_dir() {
            return path;
        }
    }
    env::current_dir().unwrap_or_else(|_| PathBuf::from("."))
}

fn strip_shell_marker(stdout: String) -> (String, Option<String>) {
    let mut cwd = None;
    let mut output = Vec::new();
    let normalized = stdout.replace("\r\n", "\n");
    for line in normalized.split('\n') {
        if let Some(value) = line.strip_prefix(SHELL_CWD_MARKER) {
            cwd = Some(value.trim().to_string());
        } else {
            output.push(line);
        }
    }
    (output.join("\n").trim_end().to_string(), cwd)
}

fn powershell_encoded_command(command: &str) -> String {
    let script = format!(
        "$Error.Clear(); {command}; $wapiExitCode = if ($global:LASTEXITCODE -ne $null) {{ $global:LASTEXITCODE }} elseif ($Error.Count) {{ 1 }} else {{ 0 }}; Write-Output \"{SHELL_CWD_MARKER}$((Get-Location).Path)\"; exit $wapiExitCode"
    );
    let bytes = script
        .encode_utf16()
        .flat_map(|unit| unit.to_le_bytes())
        .collect::<Vec<_>>();
    STANDARD.encode(bytes)
}

fn run_shell(payload: ShellPayload) -> ShellResult {
    let shell = if payload.shell.as_deref() == Some("cmd") {
        "cmd"
    } else {
        "powershell"
    };
    let command = payload.command.unwrap_or_default().trim().to_string();
    let cwd = resolve_shell_cwd(payload.cwd);
    let cwd_string = cwd.to_string_lossy().into_owned();
    if command.is_empty() {
        return ShellResult {
            ok: true,
            code: Some(0),
            stdout: String::new(),
            stderr: String::new(),
            cwd: cwd_string,
            shell: shell.into(),
        };
    }

    let (executable, args) = if shell == "cmd" {
        (
            "cmd.exe",
            vec![
                "/v:on".to_string(),
                "/d".to_string(),
                "/s".to_string(),
                "/c".to_string(),
                format!(
                    "{command} & set __WAPI_EXIT__=!ERRORLEVEL! & echo {SHELL_CWD_MARKER}%CD% & exit /b !__WAPI_EXIT__"
                ),
            ],
        )
    } else {
        (
            "powershell.exe",
            vec![
                "-NoLogo".to_string(),
                "-NoProfile".to_string(),
                "-EncodedCommand".to_string(),
                powershell_encoded_command(&command),
            ],
        )
    };

    let result = run_process(
        Path::new(executable),
        args,
        Duration::from_secs(30),
        1024 * 1024,
    );
    let (stdout, parsed_cwd) = strip_shell_marker(result.stdout);
    ShellResult {
        ok: result.ok,
        code: result.code,
        stdout,
        stderr: result.stderr.trim_end().to_string(),
        cwd: parsed_cwd.unwrap_or(cwd_string),
        shell: shell.into(),
    }
}

#[tauri::command]
pub async fn shell(payload: ShellPayload) -> Result<ShellResult, String> {
    tauri::async_runtime::spawn_blocking(move || run_shell(payload))
        .await
        .map_err(|error| format!("Shell command failed: {error}"))
}

#[cfg(windows)]
trait WindowsCommandExt {
    fn creation_flags(&mut self, flags: u32) -> &mut Self;
}

#[cfg(windows)]
impl WindowsCommandExt for Command {
    fn creation_flags(&mut self, flags: u32) -> &mut Self {
        std::os::windows::process::CommandExt::creation_flags(self, flags)
    }
}

#[cfg(not(windows))]
trait WindowsCommandExt {
    fn creation_flags(&mut self, _flags: u32) -> &mut Self;
}

#[cfg(not(windows))]
impl WindowsCommandExt for Command {
    fn creation_flags(&mut self, _flags: u32) -> &mut Self {
        self
    }
}

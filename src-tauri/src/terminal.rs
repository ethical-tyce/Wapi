use std::{
    collections::HashMap,
    env,
    io::{Read, Write},
    path::PathBuf,
    sync::{
        atomic::{AtomicU64, Ordering},
        Mutex,
    },
    thread,
};

use portable_pty::{native_pty_system, Child, CommandBuilder, MasterPty, PtySize};
use tauri::{AppHandle, Emitter, Manager, State};

use crate::models::{
    TerminalDataPayload, TerminalEvent, TerminalResizePayload, TerminalResult, TerminalStartPayload,
};

struct TerminalSession {
    master: Box<dyn MasterPty + Send>,
    writer: Box<dyn Write + Send>,
    child: Box<dyn Child + Send + Sync>,
    token: u64,
}

#[derive(Default)]
pub struct TerminalRegistry {
    sessions: Mutex<HashMap<String, TerminalSession>>,
    next_token: AtomicU64,
}

fn valid_session_id(value: &str) -> bool {
    !value.is_empty()
        && value.len() <= 80
        && value
            .chars()
            .all(|character| character.is_ascii_alphanumeric() || matches!(character, '_' | '-'))
}

fn resolve_cwd(requested: Option<String>) -> PathBuf {
    if let Some(path) = requested {
        let path = PathBuf::from(path);
        if path.is_dir() {
            return path;
        }
    }
    env::current_dir().unwrap_or_else(|_| PathBuf::from("."))
}

fn stop_session(registry: &TerminalRegistry, session_id: &str) {
    if let Ok(mut sessions) = registry.sessions.lock() {
        if let Some(mut session) = sessions.remove(session_id) {
            let _ = session.child.kill();
        }
    }
}

fn emit_reader(
    app: AppHandle,
    session_id: String,
    shell: String,
    token: u64,
    mut reader: Box<dyn Read + Send>,
) {
    thread::spawn(move || {
        let mut buffer = [0_u8; 8192];
        loop {
            match reader.read(&mut buffer) {
                Ok(0) => break,
                Ok(count) => {
                    let data = String::from_utf8_lossy(&buffer[..count]).into_owned();
                    let _ = app.emit(
                        "wapi-terminal-data",
                        TerminalEvent {
                            event_type: "data".into(),
                            session_id: session_id.clone(),
                            shell: shell.clone(),
                            data: Some(data),
                            exit_code: None,
                        },
                    );
                }
                Err(_) => break,
            }
        }

        if let Ok(mut sessions) = app.state::<TerminalRegistry>().sessions.lock() {
            let should_remove = sessions
                .get(&session_id)
                .map(|session| session.token == token)
                .unwrap_or(false);
            if should_remove {
                sessions.remove(&session_id);
            }
        }

        let _ = app.emit(
            "wapi-terminal-data",
            TerminalEvent {
                event_type: "exit".into(),
                session_id,
                shell,
                data: None,
                exit_code: None,
            },
        );
    });
}

#[tauri::command]
pub fn terminal_start(
    app: AppHandle,
    payload: TerminalStartPayload,
    registry: State<'_, TerminalRegistry>,
) -> TerminalResult {
    if !valid_session_id(&payload.session_id) {
        return TerminalResult::error("Invalid terminal session ID.");
    }

    stop_session(&registry, &payload.session_id);

    let shell = if payload.shell.as_deref() == Some("cmd") {
        "cmd"
    } else {
        "powershell"
    };
    let executable = if shell == "cmd" {
        "cmd.exe"
    } else {
        "powershell.exe"
    };
    let cwd = resolve_cwd(payload.cwd);
    let cols = payload.cols.unwrap_or(80).clamp(20, 500);
    let rows = payload.rows.unwrap_or(24).clamp(5, 200);

    let pty_system = native_pty_system();
    let pair = match pty_system.openpty(PtySize {
        rows,
        cols,
        pixel_width: 0,
        pixel_height: 0,
    }) {
        Ok(pair) => pair,
        Err(error) => return TerminalResult::error(error.to_string()),
    };

    let mut command = CommandBuilder::new(executable);
    command.cwd(cwd.clone());
    if shell == "cmd" {
        command.args(["/d", "/q"]);
    } else {
        command.args(["-NoLogo", "-NoProfile"]);
    }

    let child = match pair.slave.spawn_command(command) {
        Ok(child) => child,
        Err(error) => return TerminalResult::error(error.to_string()),
    };
    drop(pair.slave);

    let pid = child.process_id();
    let reader = match pair.master.try_clone_reader() {
        Ok(reader) => reader,
        Err(error) => return TerminalResult::error(error.to_string()),
    };
    let writer = match pair.master.take_writer() {
        Ok(writer) => writer,
        Err(error) => return TerminalResult::error(error.to_string()),
    };

    let token = registry.next_token.fetch_add(1, Ordering::Relaxed) + 1;
    let session_id = payload.session_id.clone();
    if let Ok(mut sessions) = registry.sessions.lock() {
        sessions.insert(
            session_id.clone(),
            TerminalSession {
                master: pair.master,
                writer,
                child,
                token,
            },
        );
    } else {
        return TerminalResult::error("Terminal state is unavailable.");
    }

    emit_reader(app, session_id.clone(), shell.into(), token, reader);

    TerminalResult {
        ok: true,
        session_id: Some(session_id),
        shell: Some(shell.into()),
        cwd: Some(cwd.to_string_lossy().into_owned()),
        pid,
        stderr: None,
    }
}

#[tauri::command]
pub fn terminal_send(
    payload: TerminalDataPayload,
    registry: State<'_, TerminalRegistry>,
) -> TerminalResult {
    let Ok(mut sessions) = registry.sessions.lock() else {
        return TerminalResult::error("Terminal state is unavailable.");
    };
    let Some(session) = sessions.get_mut(&payload.session_id) else {
        return TerminalResult::error("Terminal session is not running.");
    };
    match session.writer.write_all(payload.data.as_bytes()) {
        Ok(_) => {
            let _ = session.writer.flush();
            TerminalResult::ok()
        }
        Err(error) => TerminalResult::error(error.to_string()),
    }
}

#[tauri::command]
pub fn terminal_resize(
    payload: TerminalResizePayload,
    registry: State<'_, TerminalRegistry>,
) -> TerminalResult {
    let Ok(sessions) = registry.sessions.lock() else {
        return TerminalResult::error("Terminal state is unavailable.");
    };
    let Some(session) = sessions.get(&payload.session_id) else {
        return TerminalResult::error("Terminal session is not running.");
    };
    let size = PtySize {
        rows: payload.rows.unwrap_or(24).clamp(5, 200),
        cols: payload.cols.unwrap_or(80).clamp(20, 500),
        pixel_width: 0,
        pixel_height: 0,
    };
    match session.master.resize(size) {
        Ok(_) => TerminalResult::ok(),
        Err(error) => TerminalResult::error(error.to_string()),
    }
}

#[tauri::command]
pub fn terminal_stop(
    payload: TerminalDataPayload,
    registry: State<'_, TerminalRegistry>,
) -> TerminalResult {
    stop_session(&registry, &payload.session_id);
    TerminalResult::ok()
}

pub fn stop_all(registry: &TerminalRegistry) {
    if let Ok(mut sessions) = registry.sessions.lock() {
        for (_, mut session) in sessions.drain() {
            let _ = session.child.kill();
        }
    }
}

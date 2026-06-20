use std::sync::atomic::{AtomicBool, Ordering};

use tauri::{State, WebviewWindow};

#[derive(Default)]
pub struct DirtyState(pub AtomicBool);

#[tauri::command]
pub fn window_minimize(window: WebviewWindow) -> Result<(), String> {
    window.minimize().map_err(|error| error.to_string())
}

#[tauri::command]
pub fn window_toggle_maximize(window: WebviewWindow) -> Result<(), String> {
    let maximized = window.is_maximized().map_err(|error| error.to_string())?;
    if maximized {
        window.unmaximize()
    } else {
        window.maximize()
    }
    .map_err(|error| error.to_string())
}

#[tauri::command]
pub fn window_close(window: WebviewWindow) -> Result<(), String> {
    window.close().map_err(|error| error.to_string())
}

#[tauri::command]
pub fn window_set_dirty_state(
    window: WebviewWindow,
    is_dirty: bool,
    state: State<'_, DirtyState>,
) -> Result<bool, String> {
    state.0.store(is_dirty, Ordering::SeqCst);
    window
        .set_title(if is_dirty { "* Wapi IDE" } else { "Wapi IDE" })
        .map_err(|error| error.to_string())?;
    Ok(true)
}

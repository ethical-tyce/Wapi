mod models;
mod projects;
mod runtime;
mod terminal;
mod window;

use std::sync::atomic::Ordering;

use tauri::{Manager, RunEvent, WindowEvent};
use terminal::TerminalRegistry;
use window::DirtyState;

pub fn run() {
    let app = tauri::Builder::default()
        .manage(DirtyState::default())
        .manage(runtime::RunningCommands::default())
        .manage(TerminalRegistry::default())
        .invoke_handler(tauri::generate_handler![
            runtime::execute,
            runtime::locate,
            runtime::shell,
            terminal::terminal_start,
            terminal::terminal_send,
            terminal::terminal_resize,
            terminal::terminal_stop,
            projects::add_files,
            projects::create_project,
            projects::load_project,
            projects::read_project_config,
            projects::write_project_config,
            projects::list_recent_projects,
            projects::add_recent_project,
            projects::open_file,
            projects::save_file,
            projects::save_files,
            window::window_minimize,
            window::window_toggle_maximize,
            window::window_close,
            window::window_set_dirty_state,
        ])
        .on_window_event(|window, event| {
            if let WindowEvent::CloseRequested { api, .. } = event {
                let state = window.state::<DirtyState>();
                if state.0.load(Ordering::SeqCst) {
                    api.prevent_close();
                    let discard = rfd::MessageDialog::new()
                        .set_level(rfd::MessageLevel::Warning)
                        .set_title("Unsaved Wapi changes")
                        .set_description(
                            "This project has unsaved changes. Discard them and close Wapi IDE?",
                        )
                        .set_buttons(rfd::MessageButtons::YesNo)
                        .show();
                    if matches!(discard, rfd::MessageDialogResult::Yes) {
                        state.0.store(false, Ordering::SeqCst);
                        let _ = window.close();
                    }
                }
            }
        })
        .build(tauri::generate_context!())
        .expect("error while building Wapi IDE");

    app.run(|app_handle, event| {
        if matches!(event, RunEvent::Exit) {
            let registry = app_handle.state::<TerminalRegistry>();
            terminal::stop_all(&registry);
        }
    });
}

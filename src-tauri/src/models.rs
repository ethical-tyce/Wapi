use serde::{Deserialize, Serialize};

#[derive(Debug, Clone, Serialize, Deserialize)]
#[serde(rename_all = "camelCase")]
pub struct ProjectConfig {
    pub name: String,
    pub entry_file: String,
    pub default_mode: String,
    pub strict_permissions: bool,
    pub allow_injection: bool,
    pub capabilities: Vec<String>,
    #[serde(default)]
    pub json_output: bool,
}

#[derive(Debug, Clone, Serialize, Deserialize)]
#[serde(rename_all = "camelCase")]
pub struct IdeFile {
    pub file_path: Option<String>,
    pub source: String,
    pub name: String,
    pub relative_path: String,
}

#[derive(Debug, Clone, Serialize, Deserialize)]
#[serde(rename_all = "camelCase")]
pub struct ProjectPayload {
    pub name: String,
    pub config: ProjectConfig,
    #[serde(default)]
    pub files: Vec<IdeFile>,
}

#[derive(Debug, Clone, Serialize)]
#[serde(rename_all = "camelCase")]
pub struct ProjectResult {
    pub root_path: String,
    pub config: Option<ProjectConfig>,
    pub files: Vec<IdeFile>,
}

#[derive(Debug, Clone, Serialize, Deserialize)]
#[serde(rename_all = "camelCase")]
pub struct RecentProject {
    pub root_path: String,
    pub name: String,
    pub opened_at: String,
}

#[derive(Debug, Clone, Serialize, Deserialize)]
#[serde(rename_all = "camelCase")]
pub struct SaveFilePayload {
    pub file_path: Option<String>,
    pub source: String,
}

#[derive(Debug, Clone, Serialize)]
#[serde(rename_all = "camelCase")]
pub struct SaveFileResult {
    pub file_path: String,
}

#[derive(Debug, Clone, Serialize)]
#[serde(rename_all = "camelCase")]
pub struct SaveResult {
    pub ok: bool,
    pub file_path: Option<String>,
    pub error: Option<String>,
}

#[derive(Debug, Clone, Default, Serialize, Deserialize)]
#[serde(rename_all = "camelCase")]
pub struct ExecuteOptions {
    pub mode: Option<String>,
    #[serde(default)]
    pub allow_injection: bool,
    #[serde(default)]
    pub strict_permissions: bool,
    #[serde(default)]
    pub capabilities: Vec<String>,
    #[serde(default)]
    pub json_output: bool,
}

#[derive(Debug, Clone, Serialize, Deserialize)]
#[serde(rename_all = "camelCase")]
pub struct ExecutePayload {
    pub command: String,
    pub source: String,
    #[serde(default)]
    pub options: ExecuteOptions,
}

#[derive(Debug, Clone, Serialize)]
#[serde(rename_all = "camelCase")]
pub struct ExecuteResult {
    pub ok: bool,
    pub code: Option<i32>,
    pub stdout: String,
    pub stderr: String,
    pub exe: Option<String>,
    pub pid: Option<u32>,
    pub duration_ms: u128,
    pub timed_out: bool,
    pub output_truncated: bool,
    pub structured_output: Option<String>,
}

impl ExecuteResult {
    pub fn failure(message: impl Into<String>) -> Self {
        Self {
            ok: false,
            code: None,
            stdout: String::new(),
            stderr: message.into(),
            exe: None,
            pid: None,
            duration_ms: 0,
            timed_out: false,
            output_truncated: false,
            structured_output: None,
        }
    }
}

#[derive(Debug, Clone, Default, Serialize, Deserialize)]
#[serde(rename_all = "camelCase")]
pub struct ShellPayload {
    pub shell: Option<String>,
    pub command: Option<String>,
    pub cwd: Option<String>,
}

#[derive(Debug, Clone, Serialize)]
#[serde(rename_all = "camelCase")]
pub struct ShellResult {
    pub ok: bool,
    pub code: Option<i32>,
    pub stdout: String,
    pub stderr: String,
    pub cwd: String,
    pub shell: String,
}

#[derive(Debug, Clone, Default, Serialize, Deserialize)]
#[serde(rename_all = "camelCase")]
pub struct TerminalStartPayload {
    pub session_id: String,
    pub shell: Option<String>,
    pub cwd: Option<String>,
    pub cols: Option<u16>,
    pub rows: Option<u16>,
}

#[derive(Debug, Clone, Serialize, Deserialize)]
#[serde(rename_all = "camelCase")]
pub struct TerminalDataPayload {
    pub session_id: String,
    #[serde(default)]
    pub data: String,
}

#[derive(Debug, Clone, Serialize, Deserialize)]
#[serde(rename_all = "camelCase")]
pub struct TerminalResizePayload {
    pub session_id: String,
    pub cols: Option<u16>,
    pub rows: Option<u16>,
}

#[derive(Debug, Clone, Serialize)]
#[serde(rename_all = "camelCase")]
pub struct TerminalResult {
    pub ok: bool,
    pub session_id: Option<String>,
    pub shell: Option<String>,
    pub cwd: Option<String>,
    pub pid: Option<u32>,
    pub stderr: Option<String>,
}

impl TerminalResult {
    pub fn ok() -> Self {
        Self {
            ok: true,
            session_id: None,
            shell: None,
            cwd: None,
            pid: None,
            stderr: None,
        }
    }

    pub fn error(message: impl Into<String>) -> Self {
        Self {
            ok: false,
            session_id: None,
            shell: None,
            cwd: None,
            pid: None,
            stderr: Some(message.into()),
        }
    }
}

#[derive(Debug, Clone, Serialize)]
#[serde(rename_all = "camelCase")]
pub struct TerminalEvent {
    #[serde(rename = "type")]
    pub event_type: String,
    pub session_id: String,
    pub shell: String,
    pub data: Option<String>,
    pub exit_code: Option<i32>,
}

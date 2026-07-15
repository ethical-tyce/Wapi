use std::{
    collections::BTreeSet,
    fs,
    path::{Component, Path, PathBuf},
};

use chrono::Utc;
use tauri::{AppHandle, Manager};

use crate::models::{
    IdeFile, ProjectConfig, ProjectPayload, ProjectResult, RecentProject, SaveFilePayload,
    SaveFileResult, SaveResult,
};

const VISIBLE_EXTENSIONS: &[&str] = &["wapi", "txt", "json", "cpp", "c", "h", "hpp"];
const SKIPPED_DIRECTORIES: &[&str] = &[".git", "node_modules", "dist", "build", "x64", "ARM64"];

fn io_error(context: &str, error: impl std::fmt::Display) -> String {
    format!("{context}: {error}")
}

fn sanitize_project_name(name: &str) -> String {
    let cleaned: String = name
        .chars()
        .filter(|character| {
            !matches!(
                character,
                '<' | '>' | ':' | '"' | '/' | '\\' | '|' | '?' | '*' | '\0'..='\u{1f}'
            )
        })
        .collect();
    let trimmed = cleaned.trim();
    if trimmed.is_empty() {
        "WapiProject".into()
    } else {
        trimmed.into()
    }
}

fn safe_relative_path(value: &str) -> Result<PathBuf, String> {
    let path = Path::new(value);
    let mut clean = PathBuf::new();

    for component in path.components() {
        match component {
            Component::Normal(part) => clean.push(part),
            Component::CurDir => {}
            Component::ParentDir | Component::RootDir | Component::Prefix(_) => {
                return Err(format!("Invalid project file path: {value}"));
            }
        }
    }

    if clean.as_os_str().is_empty() {
        clean.push("main.wapi");
    }
    Ok(clean)
}

fn normalize_project_config(mut config: ProjectConfig, fallback_name: &str) -> ProjectConfig {
    config.name = sanitize_project_name(if config.name.trim().is_empty() {
        fallback_name
    } else {
        &config.name
    });
    config.entry_file = safe_relative_path(&config.entry_file)
        .unwrap_or_else(|_| PathBuf::from("main.wapi"))
        .to_string_lossy()
        .replace('\\', "/");
    if config.default_mode == "audit" {
        config.default_mode = "dev".into();
    }
    if !matches!(
        config.default_mode.as_str(),
        "safe" | "dev" | "unsafe" | "dangerous"
    ) {
        config.default_mode = "safe".into();
    }
    config.capabilities = config
        .capabilities
        .into_iter()
        .map(|capability| capability.trim().to_string())
        .filter(|capability| !capability.is_empty())
        .collect::<BTreeSet<_>>()
        .into_iter()
        .collect();
    config
}

fn read_ide_file(file_path: &Path, root_path: Option<&Path>) -> Result<IdeFile, String> {
    let source = fs::read_to_string(file_path)
        .map_err(|error| io_error(&format!("Could not read {}", file_path.display()), error))?;
    let name = file_path
        .file_name()
        .and_then(|value| value.to_str())
        .unwrap_or("Untitled")
        .to_string();
    let relative_path = root_path
        .and_then(|root| file_path.strip_prefix(root).ok())
        .unwrap_or(file_path)
        .to_string_lossy()
        .replace('\\', "/");

    Ok(IdeFile {
        file_path: Some(file_path.to_string_lossy().into_owned()),
        source,
        name,
        relative_path,
    })
}

fn collect_project_files(root_path: &Path) -> Result<Vec<IdeFile>, String> {
    fn walk(root: &Path, directory: &Path, files: &mut Vec<IdeFile>) -> Result<(), String> {
        let entries = fs::read_dir(directory)
            .map_err(|error| io_error(&format!("Could not read {}", directory.display()), error))?;

        for entry in entries {
            let entry = entry.map_err(|error| io_error("Could not read directory entry", error))?;
            let file_type = entry
                .file_type()
                .map_err(|error| io_error("Could not inspect directory entry", error))?;
            if file_type.is_symlink() {
                continue;
            }

            let path = entry.path();
            if file_type.is_dir() {
                let name = entry.file_name();
                if !SKIPPED_DIRECTORIES
                    .iter()
                    .any(|skipped| name.eq_ignore_ascii_case(skipped))
                {
                    walk(root, &path, files)?;
                }
                continue;
            }

            let extension = path
                .extension()
                .and_then(|value| value.to_str())
                .unwrap_or_default();
            if VISIBLE_EXTENSIONS
                .iter()
                .any(|visible| extension.eq_ignore_ascii_case(visible))
            {
                files.push(read_ide_file(&path, Some(root))?);
            }
        }
        Ok(())
    }

    let mut files = Vec::new();
    walk(root_path, root_path, &mut files)?;
    files.sort_by(|left, right| left.relative_path.cmp(&right.relative_path));
    Ok(files)
}

fn project_config_path(root_path: &Path) -> PathBuf {
    root_path.join(".wapi").join("project.json")
}

fn read_project_config_inner(root_path: &Path) -> Result<Option<ProjectConfig>, String> {
    let path = project_config_path(root_path);
    if !path.is_file() {
        return Ok(None);
    }
    let source = fs::read_to_string(&path)
        .map_err(|error| io_error(&format!("Could not read {}", path.display()), error))?;
    let config: ProjectConfig = serde_json::from_str(&source)
        .map_err(|error| io_error("Project configuration is invalid", error))?;
    let fallback = root_path
        .file_name()
        .and_then(|value| value.to_str())
        .unwrap_or("WapiProject");
    Ok(Some(normalize_project_config(config, fallback)))
}

fn write_project_config_inner(
    root_path: &Path,
    config: ProjectConfig,
) -> Result<ProjectConfig, String> {
    let fallback = root_path
        .file_name()
        .and_then(|value| value.to_str())
        .unwrap_or("WapiProject");
    let normalized = normalize_project_config(config, fallback);
    let path = project_config_path(root_path);
    if let Some(parent) = path.parent() {
        fs::create_dir_all(parent)
            .map_err(|error| io_error("Could not create .wapi directory", error))?;
    }
    let source = serde_json::to_string_pretty(&normalized)
        .map_err(|error| io_error("Could not serialize project configuration", error))?;
    fs::write(&path, format!("{source}\n"))
        .map_err(|error| io_error(&format!("Could not write {}", path.display()), error))?;
    Ok(normalized)
}

fn recent_projects_path(app: &AppHandle) -> Result<PathBuf, String> {
    app.path()
        .app_data_dir()
        .map(|path| path.join("recent-projects.json"))
        .map_err(|error| io_error("Could not resolve application data directory", error))
}

fn read_recent_projects_inner(app: &AppHandle) -> Result<Vec<RecentProject>, String> {
    let path = recent_projects_path(app)?;
    if !path.is_file() {
        return Ok(Vec::new());
    }
    let source = fs::read_to_string(&path)
        .map_err(|error| io_error("Could not read recent projects", error))?;
    let mut projects: Vec<RecentProject> = serde_json::from_str(&source).unwrap_or_default();
    projects.retain(|project| !project.root_path.trim().is_empty());
    projects.truncate(10);
    Ok(projects)
}

fn write_recent_projects_inner(app: &AppHandle, projects: &[RecentProject]) -> Result<(), String> {
    let path = recent_projects_path(app)?;
    if let Some(parent) = path.parent() {
        fs::create_dir_all(parent)
            .map_err(|error| io_error("Could not create application data directory", error))?;
    }
    let source = serde_json::to_string_pretty(&projects.iter().take(10).collect::<Vec<_>>())
        .map_err(|error| io_error("Could not serialize recent projects", error))?;
    fs::write(path, format!("{source}\n"))
        .map_err(|error| io_error("Could not save recent projects", error))
}

fn add_recent_project_inner(
    app: &AppHandle,
    root_path: &Path,
) -> Result<Vec<RecentProject>, String> {
    let resolved = fs::canonicalize(root_path).unwrap_or_else(|_| root_path.to_path_buf());
    let resolved_string = resolved.to_string_lossy().into_owned();
    let mut recent = read_recent_projects_inner(app)?;
    recent.retain(|project| {
        !Path::new(&project.root_path).eq(&resolved)
            && !project.root_path.eq_ignore_ascii_case(&resolved_string)
    });
    recent.insert(
        0,
        RecentProject {
            root_path: resolved_string,
            name: resolved
                .file_name()
                .and_then(|value| value.to_str())
                .unwrap_or("WapiProject")
                .to_string(),
            opened_at: Utc::now().to_rfc3339(),
        },
    );
    recent.truncate(10);
    write_recent_projects_inner(app, &recent)?;
    Ok(recent)
}

fn unique_project_path(parent: &Path, project_name: &str) -> Result<PathBuf, String> {
    for index in 0..50 {
        let suffix = if index == 0 {
            String::new()
        } else {
            format!(" {}", index + 1)
        };
        let candidate = parent.join(format!("{project_name}{suffix}"));
        if !candidate.exists() {
            return Ok(candidate);
        }
    }
    Err(format!(
        "Could not create a unique folder for {project_name}"
    ))
}

fn write_project_files(root_path: &Path, files: &[IdeFile]) -> Result<(), String> {
    for file in files {
        let relative = safe_relative_path(if file.relative_path.trim().is_empty() {
            &file.name
        } else {
            &file.relative_path
        })?;
        let target = root_path.join(relative);
        if let Some(parent) = target.parent() {
            fs::create_dir_all(parent)
                .map_err(|error| io_error("Could not create project directory", error))?;
        }
        fs::write(&target, &file.source)
            .map_err(|error| io_error(&format!("Could not write {}", target.display()), error))?;
    }
    Ok(())
}

#[tauri::command]
pub fn add_files() -> Result<Vec<IdeFile>, String> {
    let Some(paths) = rfd::FileDialog::new()
        .set_title("Add files to solution")
        .add_filter("Wapi and source files", VISIBLE_EXTENSIONS)
        .pick_files()
    else {
        return Ok(Vec::new());
    };

    paths.iter().map(|path| read_ide_file(path, None)).collect()
}

#[tauri::command]
pub fn create_project(
    app: AppHandle,
    payload: ProjectPayload,
) -> Result<Option<ProjectResult>, String> {
    let project_name = sanitize_project_name(&payload.name);
    let Some(parent) = rfd::FileDialog::new()
        .set_title(format!("Choose where to create {project_name}"))
        .pick_folder()
    else {
        return Ok(None);
    };

    let root_path = unique_project_path(&parent, &project_name)?;
    fs::create_dir(&root_path)
        .map_err(|error| io_error("Could not create project directory", error))?;

    let files = if payload.files.is_empty() {
        vec![IdeFile {
            file_path: None,
            source: "listProcesses()\n".into(),
            name: "main.wapi".into(),
            relative_path: "main.wapi".into(),
        }]
    } else {
        payload.files
    };

    write_project_files(&root_path, &files)?;
    write_project_config_inner(&root_path, payload.config)?;
    add_recent_project_inner(&app, &root_path)?;

    Ok(Some(ProjectResult {
        root_path: root_path.to_string_lossy().into_owned(),
        config: read_project_config_inner(&root_path)?,
        files: collect_project_files(&root_path)?,
    }))
}

#[tauri::command]
pub fn load_project(
    app: AppHandle,
    root_path: Option<String>,
) -> Result<Option<ProjectResult>, String> {
    let root_path = match root_path.filter(|path| !path.trim().is_empty()) {
        Some(path) => PathBuf::from(path),
        None => {
            let Some(path) = rfd::FileDialog::new()
                .set_title("Upload project folder")
                .pick_folder()
            else {
                return Ok(None);
            };
            path
        }
    };

    if !root_path.is_dir() {
        return Ok(None);
    }

    add_recent_project_inner(&app, &root_path)?;
    Ok(Some(ProjectResult {
        root_path: root_path.to_string_lossy().into_owned(),
        config: read_project_config_inner(&root_path)?,
        files: collect_project_files(&root_path)?,
    }))
}

#[tauri::command]
pub fn read_project_config(root_path: String) -> Result<Option<ProjectConfig>, String> {
    read_project_config_inner(Path::new(&root_path))
}

#[tauri::command]
pub fn write_project_config(
    root_path: String,
    config: ProjectConfig,
) -> Result<ProjectConfig, String> {
    write_project_config_inner(Path::new(&root_path), config)
}

#[tauri::command]
pub fn list_recent_projects(app: AppHandle) -> Result<Vec<RecentProject>, String> {
    read_recent_projects_inner(&app)
}

#[tauri::command]
pub fn add_recent_project(app: AppHandle, root_path: String) -> Result<Vec<RecentProject>, String> {
    if root_path.trim().is_empty() {
        return read_recent_projects_inner(&app);
    }
    add_recent_project_inner(&app, Path::new(&root_path))
}

#[tauri::command]
pub fn open_file() -> Result<Option<IdeFile>, String> {
    let Some(path) = rfd::FileDialog::new()
        .set_title("Open Wapi script")
        .add_filter("Wapi scripts", &["wapi", "txt"])
        .pick_file()
    else {
        return Ok(None);
    };
    read_ide_file(&path, None).map(Some)
}

#[tauri::command]
pub fn save_file(payload: SaveFilePayload) -> Result<Option<SaveFileResult>, String> {
    let path = match payload.file_path.filter(|path| !path.trim().is_empty()) {
        Some(path) => PathBuf::from(path),
        None => {
            let Some(path) = rfd::FileDialog::new()
                .set_title("Save Wapi script")
                .set_file_name("script.wapi")
                .add_filter("Wapi scripts", &["wapi"])
                .save_file()
            else {
                return Ok(None);
            };
            path
        }
    };

    fs::write(&path, payload.source)
        .map_err(|error| io_error(&format!("Could not write {}", path.display()), error))?;
    Ok(Some(SaveFileResult {
        file_path: path.to_string_lossy().into_owned(),
    }))
}

#[tauri::command]
pub fn save_files(files: Vec<IdeFile>) -> Vec<SaveResult> {
    files
        .into_iter()
        .map(|file| {
            let Some(path) = file.file_path.filter(|path| !path.trim().is_empty()) else {
                return SaveResult {
                    ok: false,
                    file_path: None,
                    error: Some("Missing file path.".into()),
                };
            };
            match fs::write(&path, file.source) {
                Ok(_) => SaveResult {
                    ok: true,
                    file_path: Some(path),
                    error: None,
                },
                Err(error) => SaveResult {
                    ok: false,
                    file_path: Some(path),
                    error: Some(error.to_string()),
                },
            }
        })
        .collect()
}

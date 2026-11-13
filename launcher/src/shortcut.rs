//! Desktop and Start-menu shortcuts that start a profile directly.
//!
//! The shortcut runs this launcher with `--profile "<name>" --launch` (see `main.rs`), so the
//! profile is looked up by name when it is used: renaming the profile breaks its shortcuts,
//! and the launcher then opens normally with a message saying so.

use std::path::{Path, PathBuf};

#[derive(Clone, Copy, Debug, PartialEq, Eq)]
pub enum Place {
    Desktop,
    StartMenu,
}

/// Creates (or replaces) a shortcut that runs the launcher with
/// `--profile "<name>" --launch`. Returns the shortcut's path.
pub fn create(profile_name: &str, place: Place) -> Result<PathBuf, String> {
    create_in(profile_name, &folder(place)?)
}

/// The folder shortcuts for `place` go in.
pub fn folder(place: Place) -> Result<PathBuf, String> {
    match place {
        Place::Desktop => dirs::desktop_dir().ok_or_else(|| "The Desktop folder couldn't be found.".to_owned()),
        Place::StartMenu => std::env::var_os("APPDATA")
            .map(PathBuf::from)
            .filter(|p| p.is_dir())
            .or_else(dirs::config_dir)
            .map(|appdata| appdata.join("Microsoft").join("Windows").join("Start Menu").join("Programs").join("Command Center"))
            .ok_or_else(|| "The Start menu folder couldn't be found.".to_owned()),
    }
}

/// Like [`create`], into a folder of your choosing.
pub fn create_in(profile_name: &str, dir: &Path) -> Result<PathBuf, String> {
    let exe = std::env::current_exe().map_err(|e| format!("Couldn't work out where the launcher is: {e}"))?;
    std::fs::create_dir_all(dir).map_err(|e| format!("Couldn't create {}: {e}", dir.display()))?;
    let path = dir.join(file_name(profile_name));
    let link = Link {
        work_dir: exe.parent().map(Path::to_path_buf).unwrap_or_default(),
        icon: exe.clone(),
        target: exe,
        arguments: arguments(profile_name),
        description: format!("Start Empire at War with the profile \"{}\"", profile_name.trim()),
    };
    write_link(&path, &link).map_err(|e| format!("Couldn't create the shortcut {}: {e}", path.display()))?;
    Ok(path)
}

/// `Play <profile> (Empire at War).lnk`, with anything Windows won't accept in a file name
/// replaced.
pub fn file_name(profile_name: &str) -> String {
    let mut safe: String = profile_name
        .trim()
        .chars()
        .map(|c| if c.is_control() || r#"<>:"/\|?*"#.contains(c) { '_' } else { c })
        .take(80)
        .collect();
    // A name can't end in a dot or a space; ours never does (it ends in the suffix), but
    // tidy up so "Play X . (Empire at War)" doesn't happen either.
    safe = safe.trim_matches(|c| c == ' ' || c == '.').to_owned();
    if safe.is_empty() {
        safe = "profile".to_owned();
    }
    format!("Play {safe} (Empire at War).lnk")
}

/// The command line after the exe: `--profile "<name>" --launch`.
pub fn arguments(profile_name: &str) -> String {
    format!("--profile {} --launch", quote(profile_name.trim()))
}

/// Quotes one argument the way Windows programs split their command line
/// (`CommandLineToArgvW`): always in double quotes, with `"` and the backslashes in front
/// of a quote escaped.
fn quote(arg: &str) -> String {
    let mut out = String::with_capacity(arg.len() + 2);
    out.push('"');
    let mut backslashes = 0;
    for c in arg.chars() {
        match c {
            '\\' => backslashes += 1,
            '"' => {
                // The backslashes so far would escape this quote: double them, then escape it.
                out.extend(std::iter::repeat('\\').take(backslashes + 1));
                backslashes = 0;
            }
            _ => backslashes = 0,
        }
        out.push(c);
    }
    // Trailing backslashes would escape the closing quote.
    out.extend(std::iter::repeat('\\').take(backslashes));
    out.push('"');
    out
}

struct Link {
    target: PathBuf,
    arguments: String,
    work_dir: PathBuf,
    icon: PathBuf,
    description: String,
}

/// Writes the `.lnk` through the shell's own `IShellLinkW`. Done on a thread of its own so
/// the COM apartment it needs never clashes with the one the calling thread already has.
#[cfg(windows)]
fn write_link(path: &Path, link: &Link) -> Result<(), String> {
    use windows::core::{Interface, HSTRING};
    use windows::Win32::System::Com::{
        CoCreateInstance, CoInitializeEx, CoUninitialize, IPersistFile, CLSCTX_INPROC_SERVER, COINIT_APARTMENTTHREADED,
    };
    use windows::Win32::UI::Shell::{IShellLinkW, ShellLink};

    let wide = |p: &Path| HSTRING::from(p.as_os_str());
    let (file, target, work_dir, icon) = (wide(path), wide(&link.target), wide(&link.work_dir), wide(&link.icon));
    let (arguments, description) = (HSTRING::from(link.arguments.as_str()), HSTRING::from(link.description.as_str()));

    let job = move || -> Result<(), String> {
        // SAFETY: plain COM calls with valid, NUL-terminated strings that outlive them; COM
        // is initialised for this thread first and uninitialised after the objects are gone.
        unsafe {
            CoInitializeEx(None, COINIT_APARTMENTTHREADED).ok().map_err(|e| e.message())?;
            let result = (|| -> windows::core::Result<()> {
                let shell_link: IShellLinkW = CoCreateInstance(&ShellLink, None, CLSCTX_INPROC_SERVER)?;
                shell_link.SetPath(&target)?;
                shell_link.SetArguments(&arguments)?;
                shell_link.SetWorkingDirectory(&work_dir)?;
                shell_link.SetIconLocation(&icon, 0)?;
                shell_link.SetDescription(&description)?;
                let persist: IPersistFile = shell_link.cast()?;
                persist.Save(&file, true)
            })();
            CoUninitialize();
            result.map_err(|e| e.message())
        }
    };
    std::thread::spawn(job).join().map_err(|_| "the shortcut writer stopped unexpectedly".to_owned())?
}

#[cfg(not(windows))]
fn write_link(_path: &Path, _link: &Link) -> Result<(), String> {
    Err("Shortcuts are only available on Windows.".into())
}

#[cfg(test)]
mod tests {
    use super::*;

    #[test]
    fn shortcut_file_names_are_valid() {
        assert_eq!(file_name("Vanilla"), "Play Vanilla (Empire at War).lnk");
        assert_eq!(file_name("  Thrawn's Revenge: 3.4  "), "Play Thrawn's Revenge_ 3.4 (Empire at War).lnk");
        assert_eq!(file_name(r#"a<b>c:d"e/f\g|h?i*j"#), "Play a_b_c_d_e_f_g_h_i_j (Empire at War).lnk");
        assert_eq!(file_name("tab\there"), "Play tab_here (Empire at War).lnk");
        assert_eq!(file_name(""), "Play profile (Empire at War).lnk");
        assert_eq!(file_name(" .. "), "Play profile (Empire at War).lnk");
        assert_eq!(file_name("Ends with dots..."), "Play Ends with dots (Empire at War).lnk");
        // Unicode names are fine as they are.
        assert_eq!(file_name("Klonkriege – Ära"), "Play Klonkriege – Ära (Empire at War).lnk");
        // Very long names are cut so the path stays usable.
        let long = file_name(&"x".repeat(300));
        assert_eq!(long.chars().count(), "Play  (Empire at War).lnk".len() + 80);
        for name in [file_name("a/b"), file_name("COM1?"), long] {
            assert!(!name.chars().any(|c| r#"<>:"/\|?*"#.contains(c) || c.is_control()), "{name}");
        }
    }

    #[test]
    fn shortcut_arguments_quote_the_profile_name() {
        assert_eq!(arguments("Vanilla"), r#"--profile "Vanilla" --launch"#);
        assert_eq!(arguments("  Awakening of the Rebellion  "), r#"--profile "Awakening of the Rebellion" --launch"#);
        assert_eq!(arguments(r#"The "big" one"#), r#"--profile "The \"big\" one" --launch"#);
        assert_eq!(arguments(r"ends with slash\"), r#"--profile "ends with slash\\" --launch"#);
        assert_eq!(arguments(r#"slash\"quote"#), r#"--profile "slash\\\"quote" --launch"#);
        assert_eq!(arguments(r"C:\not\escaped"), r#"--profile "C:\not\escaped" --launch"#);
    }

    /// What the launcher gets back when Windows splits the shortcut's command line must be
    /// the profile name that went in, whatever characters it has.
    #[test]
    fn shortcut_arguments_survive_the_command_line() {
        for name in ["Vanilla", "Two words", r#"The "big" one"#, r"trailing\", r#"mix\"ed\\"#, "ünïcödé", "a  b"] {
            let line = format!("launcher.exe {}", arguments(name));
            let args = split_command_line(&line);
            let cli = crate::parse_cli(&args[1..]);
            assert_eq!(cli.profile.as_deref(), Some(name), "command line: {line}");
            assert!(cli.launch);
        }
    }

    /// `CommandLineToArgvW`, as documented: backslashes are literal unless they come before
    /// a quote; 2n backslashes + quote = n backslashes and a quote that toggles quoting;
    /// 2n+1 backslashes + quote = n backslashes and a literal quote.
    fn split_command_line(line: &str) -> Vec<String> {
        let mut out = Vec::new();
        let mut cur = String::new();
        let mut in_quotes = false;
        let mut any = false;
        let mut chars = line.chars().peekable();
        while let Some(c) = chars.next() {
            match c {
                '\\' => {
                    let mut n = 1;
                    while chars.peek() == Some(&'\\') {
                        chars.next();
                        n += 1;
                    }
                    if chars.peek() == Some(&'"') {
                        cur.extend(std::iter::repeat('\\').take(n / 2));
                        if n % 2 == 1 {
                            chars.next();
                            cur.push('"');
                        }
                    } else {
                        cur.extend(std::iter::repeat('\\').take(n));
                    }
                    any = true;
                }
                '"' => {
                    in_quotes = !in_quotes;
                    any = true;
                }
                c if c.is_whitespace() && !in_quotes => {
                    if any {
                        out.push(std::mem::take(&mut cur));
                        any = false;
                    }
                }
                c => {
                    cur.push(c);
                    any = true;
                }
            }
        }
        if any {
            out.push(cur);
        }
        out
    }

    /// Makes a real `.lnk` through COM in a scratch folder, reads it back, and removes it.
    #[cfg(windows)]
    #[test]
    fn shortcut_is_created_for_real() {
        use windows::core::{Interface, HSTRING};
        use windows::Win32::System::Com::{
            CoCreateInstance, CoInitializeEx, CoUninitialize, IPersistFile, CLSCTX_INPROC_SERVER,
            COINIT_APARTMENTTHREADED, STGM_READ,
        };
        use windows::Win32::UI::Shell::{IShellLinkW, ShellLink};

        let dir = std::env::temp_dir().join(format!("command-center-shortcut-test-{}", std::process::id()));
        let _ = std::fs::remove_dir_all(&dir);
        let name = r#"Thrawn's "Revenge": 3.4"#;
        let path = create_in(name, &dir).expect("the shortcut should be created");
        assert_eq!(path, dir.join(r#"Play Thrawn's _Revenge__ 3.4 (Empire at War).lnk"#));
        let size = std::fs::metadata(&path).expect("the .lnk should exist").len();
        assert!(size > 100, "a .lnk of {size} bytes can't be right");
        // Creating it again replaces it rather than failing.
        assert_eq!(create_in(name, &dir).unwrap(), path);

        let file = HSTRING::from(path.as_os_str());
        let read = std::thread::spawn(move || unsafe {
            CoInitializeEx(None, COINIT_APARTMENTTHREADED).ok().unwrap();
            let out = (|| -> windows::core::Result<(String, String, String)> {
                let link: IShellLinkW = CoCreateInstance(&ShellLink, None, CLSCTX_INPROC_SERVER)?;
                link.cast::<IPersistFile>()?.Load(&file, STGM_READ)?;
                let text = |buf: &[u16]| String::from_utf16_lossy(buf).trim_end_matches('\0').to_owned();
                let mut buf = vec![0u16; 2048];
                link.GetArguments(&mut buf)?;
                let args = text(&buf);
                buf.fill(0);
                link.GetWorkingDirectory(&mut buf)?;
                let work_dir = text(&buf);
                buf.fill(0);
                let mut icon_index = -1;
                link.GetIconLocation(&mut buf, &mut icon_index)?;
                assert_eq!(icon_index, 0);
                Ok((args, work_dir, text(&buf)))
            })();
            CoUninitialize();
            out
        })
        .join()
        .unwrap()
        .expect("the .lnk should load again");

        let exe = std::env::current_exe().unwrap();
        assert_eq!(read.0, arguments(name));
        assert_eq!(Path::new(&read.1), exe.parent().unwrap());
        assert_eq!(Path::new(&read.2), exe);

        std::fs::remove_dir_all(&dir).expect("the scratch folder should be removable");
        assert!(!path.exists());
    }
}

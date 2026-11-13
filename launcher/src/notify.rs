//! Windows notifications (game exited / crashed), and the watcher that makes sure the
//! launcher reacts to the game exiting even while its own window is minimised.
//!
//! Everything here fails soft: a notification that can't be shown is simply not shown, and
//! none of it runs on the UI thread.

use crate::runner;
use std::sync::atomic::{AtomicBool, Ordering};
use std::sync::Arc;
use std::time::{Duration, Instant};

/// The identity notifications are shown under. Registered for the current user (name and
/// icon) the first time one is shown; an unpackaged program has no identity otherwise.
const APP_ID: &str = "CommandCenter.EmpireAtWarLauncher";
const APP_NAME: &str = "Command Center";

/// Title and body for the "game has exited" notification. `problem` is the one-sentence
/// description of a crash or error exit (`LastExit::summary()`), `None` for a normal exit.
pub fn exit_message(problem: Option<&str>, seconds: u64, profile: &str) -> (String, String) {
    let played = match seconds {
        0..=59 => "under a minute".to_owned(),
        s => crate::theme::fmt_duration(s),
    };
    match problem {
        Some(problem) => (
            "Empire at War stopped unexpectedly".to_owned(),
            format!("{} Played {played} with \"{profile}\".", problem.trim()),
        ),
        None => (format!("Empire at War closed \u{2014} played {played}"), format!("Profile \"{profile}\".")),
    }
}

/// What an exit code means, in the words `LastExit::summary()` uses. `None` = a normal exit.
pub fn problem_for_code(code: i32) -> Option<String> {
    match (code, runner::crash_name(code)) {
        (_, Some(name)) => Some(format!("Crashed: {name} (0x{:08X}).", code as u32)),
        (0, None) => None,
        (c, None) => Some(format!("Exited with error code {c} (0x{:08X}).", c as u32)),
    }
}

/// Shows a notification. Returns at once; any failure is ignored.
pub fn show(title: &str, body: &str) {
    let (title, body) = (title.to_owned(), body.to_owned());
    std::thread::spawn(move || {
        let _ = show_blocking(&title, &body);
    });
}

fn toast_xml(title: &str, body: &str) -> String {
    let esc = |s: &str| {
        s.chars()
            .filter(|c| !c.is_control())
            .map(|c| match c {
                '&' => "&amp;".to_owned(),
                '<' => "&lt;".to_owned(),
                '>' => "&gt;".to_owned(),
                '"' => "&quot;".to_owned(),
                '\'' => "&apos;".to_owned(),
                c => c.to_string(),
            })
            .collect::<String>()
    };
    format!(
        "<toast><visual><binding template=\"ToastGeneric\"><text>{}</text><text>{}</text></binding></visual></toast>",
        esc(title),
        esc(body)
    )
}

#[cfg(windows)]
fn show_blocking(title: &str, body: &str) -> Result<(), String> {
    use windows::core::HSTRING;
    use windows::Data::Xml::Dom::XmlDocument;
    use windows::UI::Notifications::{ToastNotification, ToastNotificationManager};

    // Without the registration Windows drops the notification silently, so there is no
    // point going on if it fails.
    register()?;
    let xml = toast_xml(title, body);
    (|| -> windows::core::Result<()> {
        let doc = XmlDocument::new()?;
        doc.LoadXml(&HSTRING::from(xml.as_str()))?;
        let toast = ToastNotification::CreateToastNotification(&doc)?;
        ToastNotificationManager::CreateToastNotifierWithId(&HSTRING::from(APP_ID))?.Show(&toast)
    })()
    .map_err(|e| e.message())
}

#[cfg(not(windows))]
fn show_blocking(_title: &str, _body: &str) -> Result<(), String> {
    Err("Notifications are only available on Windows.".into())
}

/// Tells Windows the name and icon to show notifications under:
/// `HKCU\Software\Classes\AppUserModelId\<APP_ID>`. Per user, no administrator rights.
#[cfg(windows)]
fn register() -> Result<(), String> {
    use winreg::enums::HKEY_CURRENT_USER;
    use winreg::RegKey;

    let (key, _) = RegKey::predef(HKEY_CURRENT_USER)
        .create_subkey(format!("Software\\Classes\\AppUserModelId\\{APP_ID}"))
        .map_err(|e| e.to_string())?;
    key.set_value("DisplayName", &APP_NAME).map_err(|e| e.to_string())?;
    // The icon has to be a file; keep a copy of ours in the data folder.
    let icon = crate::config::data_dir().join("notification-icon.png");
    if !icon.is_file() {
        let _ = std::fs::write(&icon, crate::app::ICON_PNG);
    }
    if icon.is_file() {
        let _ = key.set_value("IconUri", &icon.to_string_lossy().into_owned());
    }
    Ok(())
}

// ------------------------------------------------------------------ exit watcher

/// What the launcher and the watcher thread tell each other about one run of the game.
pub struct ExitWatch {
    /// Set by the launcher once it has dealt with the game exiting.
    pub handled: Arc<AtomicBool>,
    /// Set by whoever shows the exit notification first, so it is shown once.
    pub notified: Arc<AtomicBool>,
}

pub struct WatchRequest {
    /// The game's process id.
    pub pid: u32,
    /// The launcher's main window (a Win32 `HWND`), if known.
    pub hwnd: Option<isize>,
    /// The launcher minimised itself for this run and wants its window back afterwards.
    pub restore: bool,
    /// Show the exit notification if the launcher itself doesn't get to it.
    pub notify: bool,
    pub profile: String,
}

/// Waits for the game on a thread of its own. The launcher normally notices the exit itself
/// within a frame and sets `handled`; but a minimised window may not be asked to draw, and
/// then nothing in the launcher runs. In that case this thread brings the window back (if
/// the launcher minimised it), or shows the exit notification itself.
#[cfg(windows)]
pub fn watch(req: WatchRequest) -> ExitWatch {
    use windows::Win32::Foundation::{CloseHandle, HWND};
    use windows::Win32::System::Threading::{
        GetExitCodeProcess, OpenProcess, WaitForSingleObject, INFINITE, PROCESS_QUERY_LIMITED_INFORMATION,
        PROCESS_SYNCHRONIZE,
    };
    use windows::Win32::UI::WindowsAndMessaging::{IsIconic, SetForegroundWindow, ShowWindow, SW_RESTORE};

    let watch = ExitWatch { handled: Arc::new(AtomicBool::new(false)), notified: Arc::new(AtomicBool::new(false)) };
    let (handled, notified) = (watch.handled.clone(), watch.notified.clone());
    std::thread::spawn(move || {
        let started = Instant::now();
        // SAFETY: the handle is opened here, used only while open, and closed once.
        let code = unsafe {
            let Ok(process) = OpenProcess(PROCESS_SYNCHRONIZE | PROCESS_QUERY_LIMITED_INFORMATION, false, req.pid) else {
                return;
            };
            WaitForSingleObject(process, INFINITE);
            let mut code = 0u32;
            let known = GetExitCodeProcess(process, &mut code).is_ok();
            let _ = CloseHandle(process);
            known.then_some(code as i32)
        };
        let seconds = started.elapsed().as_secs();
        // Give the launcher's own loop a moment to deal with it first.
        for _ in 0..20 {
            if handled.load(Ordering::SeqCst) {
                return;
            }
            std::thread::sleep(Duration::from_millis(100));
        }
        if req.restore {
            if let Some(hwnd) = req.hwnd {
                // SAFETY: plain window calls; a stale handle just makes them fail.
                unsafe {
                    let hwnd = HWND(hwnd as *mut core::ffi::c_void);
                    if IsIconic(hwnd).as_bool() {
                        let _ = ShowWindow(hwnd, SW_RESTORE);
                    }
                    let _ = SetForegroundWindow(hwnd);
                }
                // The window draws again now, so the launcher takes it from here.
                return;
            }
        }
        if req.notify && !notified.swap(true, Ordering::SeqCst) {
            let problem = code.and_then(problem_for_code);
            let (title, body) = exit_message(problem.as_deref(), seconds, &req.profile);
            let _ = show_blocking(&title, &body);
        }
    });
    watch
}

#[cfg(not(windows))]
pub fn watch(_req: WatchRequest) -> ExitWatch {
    ExitWatch { handled: Arc::new(AtomicBool::new(false)), notified: Arc::new(AtomicBool::new(false)) }
}

#[cfg(test)]
mod tests {
    use super::*;

    #[test]
    fn notify_messages_read_well() {
        let (title, body) = exit_message(None, 4320, "Vanilla");
        assert_eq!(title, "Empire at War closed \u{2014} played 1 h 12 min");
        assert_eq!(body, "Profile \"Vanilla\".");
        assert_eq!(exit_message(None, 0, "X").0, "Empire at War closed \u{2014} played under a minute");
        assert_eq!(exit_message(None, 59, "X").0, "Empire at War closed \u{2014} played under a minute");
        assert_eq!(exit_message(None, 600, "X").0, "Empire at War closed \u{2014} played 10 min");

        let problem = problem_for_code(0xC0000005u32 as i32).unwrap();
        assert_eq!(problem, "Crashed: access violation (0xC0000005).");
        let (title, body) = exit_message(Some(&problem), 125, "AotR");
        assert_eq!(title, "Empire at War stopped unexpectedly");
        assert_eq!(body, "Crashed: access violation (0xC0000005). Played 2 min with \"AotR\".");

        assert_eq!(problem_for_code(0), None);
        assert_eq!(problem_for_code(2).unwrap(), "Exited with error code 2 (0x00000002).");
    }

    /// The watcher's wording must match what the Game console shows for the same exit.
    #[test]
    fn notify_matches_the_runner_summary() {
        for code in [1, 2, 0xC0000005u32 as i32, 0xC00000FDu32 as i32, -1] {
            let exit = runner::LastExit {
                code: Some(code),
                stopped_by_user: false,
                ran_for: Duration::from_secs(1),
                when: std::time::SystemTime::now(),
                log_path: None,
            };
            assert_eq!(problem_for_code(code).unwrap(), exit.summary());
            assert!(exit.crashed());
        }
    }

    #[test]
    fn notify_xml_is_escaped() {
        let xml = toast_xml("A & B <closed>", "Profile \"Q's\"\u{7}\n");
        assert_eq!(
            xml,
            "<toast><visual><binding template=\"ToastGeneric\"><text>A &amp; B &lt;closed&gt;</text>\
             <text>Profile &quot;Q&apos;s&quot;</text></binding></visual></toast>"
        );
    }

    /// Shows one real notification. Not part of the normal run (it pops up on the desktop
    /// and registers the launcher's notification identity for the current user):
    /// `cargo test notify_shows_for_real -- --ignored`.
    #[cfg(windows)]
    #[test]
    #[ignore]
    fn notify_shows_for_real() {
        show_blocking("Command Center", "Notification test: this is what a game exit looks like.").unwrap();
        // Give Windows a moment to take it before the test process goes away.
        std::thread::sleep(Duration::from_secs(2));
    }
}

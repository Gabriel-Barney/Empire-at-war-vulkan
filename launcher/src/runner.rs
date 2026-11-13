//! Runs the game as a child process, captures its output and records how it ended.

use crate::config;
use std::collections::VecDeque;
use std::io::{BufRead, BufReader, Read, Write};
use std::path::{Path, PathBuf};
use std::process::{Child, Command, Stdio};
use std::sync::mpsc::{channel, Receiver, Sender};
use std::sync::{Arc, Mutex};
use std::time::{Duration, Instant, SystemTime};

const MAX_LINES: usize = 20_000;

#[derive(Clone, Copy, PartialEq, Eq, Debug)]
pub enum LineKind {
    Out,
    Err,
    Launcher,
}

#[derive(Clone, Debug)]
pub struct ConsoleLine {
    pub kind: LineKind,
    pub text: String,
    /// Seconds since the game was started.
    pub at: f32,
}

#[derive(Clone, Debug)]
pub struct LastExit {
    pub code: Option<i32>,
    pub stopped_by_user: bool,
    pub ran_for: Duration,
    pub when: SystemTime,
    pub log_path: Option<PathBuf>,
}

impl LastExit {
    pub fn crashed(&self) -> bool {
        !self.stopped_by_user && self.code.map(|c| crash_name(c).is_some() || c != 0).unwrap_or(true)
    }

    /// One plain sentence describing the outcome.
    pub fn summary(&self) -> String {
        if self.stopped_by_user {
            return "Stopped from the launcher.".into();
        }
        match self.code {
            Some(0) => "Exited normally (code 0).".into(),
            Some(c) => match crash_name(c) {
                Some(name) => format!("Crashed: {name} (0x{:08X}).", c as u32),
                None => format!("Exited with error code {c} (0x{:08X}).", c as u32),
            },
            None => "Ended without an exit code.".into(),
        }
    }
}

/// Windows NTSTATUS codes that mean the process died rather than exited.
pub fn crash_name(code: i32) -> Option<&'static str> {
    Some(match code as u32 {
        0xC0000005 => "access violation",
        0xC00000FD => "stack overflow",
        0xC0000409 => "stack buffer overrun / fail-fast",
        0xC000001D => "illegal instruction",
        0xC0000094 => "integer divide by zero",
        0xC0000096 => "privileged instruction",
        0xC0000374 => "heap corruption",
        0xC0000135 => "a required DLL was not found",
        0xC0000139 => "DLL entry point not found",
        0xC0000142 => "DLL initialisation failed",
        0xC000007B => "invalid image (32/64-bit DLL mismatch)",
        0xC0000017 => "out of memory",
        0xC000013A => "terminated by Ctrl+C",
        0x80000003 => "breakpoint / assertion",
        0xE06D7363 => "unhandled C++ exception",
        0x40000015 => "fatal application exit (abort)",
        _ => return None,
    })
}

enum Event {
    Line(LineKind, String),
    Exited(Option<i32>),
}

pub struct Finished {
    pub profile_id: u64,
    pub seconds: u64,
}

/// Exactly what the game was last started with.
#[derive(Clone, Debug)]
pub struct RunInfo {
    pub exe: PathBuf,
    /// The arguments as passed, one per element.
    pub args: Vec<String>,
    pub work_dir: PathBuf,
    pub profile_id: u64,
    pub profile_name: String,
    /// Mod folders in load order, taken from the `MODPATH=` arguments.
    pub mods: Vec<PathBuf>,
    pub started: SystemTime,
}

impl RunInfo {
    /// The full command line the way it would be typed in a shell.
    pub fn command_line(&self) -> String {
        let exe = display_args(&[self.exe.to_string_lossy().into_owned()]);
        if self.args.is_empty() {
            exe
        } else {
            format!("{exe} {}", display_args(&self.args))
        }
    }
}

/// The mod folders an argument list names with `MODPATH=`, in order.
pub fn mods_from_args(args: &[String]) -> Vec<PathBuf> {
    args.iter().filter_map(|a| a.strip_prefix("MODPATH=")).filter(|p| !p.is_empty()).map(PathBuf::from).collect()
}

#[derive(Default)]
pub struct Runner {
    child: Option<Arc<Mutex<Child>>>,
    rx: Option<Receiver<Event>>,
    pub lines: VecDeque<ConsoleLine>,
    started: Option<Instant>,
    profile_id: u64,
    stop_requested: bool,
    log: Option<std::io::BufWriter<std::fs::File>>,
    log_path: Option<PathBuf>,
    pub last_exit: Option<LastExit>,
    /// Bumped whenever `lines` changes, so views can cache filtered results.
    pub revision: u64,
    /// What the current (or most recent) run was started with. `None` until the first run.
    pub last_run: Option<RunInfo>,
    /// How many times the game has been started this session; identifies a run.
    pub runs: u64,
}

pub fn logs_dir() -> PathBuf {
    let d = config::data_dir().join("logs");
    let _ = std::fs::create_dir_all(&d);
    d
}

impl Runner {
    pub fn is_running(&self) -> bool {
        self.child.is_some()
    }

    pub fn running_profile(&self) -> Option<u64> {
        self.is_running().then_some(self.profile_id)
    }

    /// Process id of the running game.
    pub fn pid(&self) -> Option<u32> {
        self.child.as_ref().and_then(|c| c.lock().ok().map(|c| c.id()))
    }

    pub fn elapsed(&self) -> Duration {
        self.started.map(|s| s.elapsed()).unwrap_or_default()
    }

    fn push(&mut self, kind: LineKind, text: String) {
        let at = self.elapsed().as_secs_f32();
        if let Some(log) = &mut self.log {
            let tag = match kind {
                LineKind::Out => ' ',
                LineKind::Err => '!',
                LineKind::Launcher => '#',
            };
            let _ = writeln!(log, "[{at:9.3}]{tag} {text}");
        }
        self.lines.push_back(ConsoleLine { kind, text, at });
        if self.lines.len() > MAX_LINES {
            self.lines.pop_front();
        }
        self.revision += 1;
    }

    pub fn clear(&mut self) {
        self.lines.clear();
        self.revision += 1;
    }

    pub fn start(
        &mut self,
        exe: &Path,
        args: &[String],
        work_dir: &Path,
        profile_id: u64,
        profile_name: &str,
        ctx: &egui::Context,
    ) -> Result<(), String> {
        if self.is_running() {
            return Err("The game is already running.".into());
        }
        let mut cmd = Command::new(exe);
        cmd.args(args)
            .current_dir(work_dir)
            .stdin(Stdio::null())
            .stdout(Stdio::piped())
            .stderr(Stdio::piped());
        #[cfg(windows)]
        {
            use std::os::windows::process::CommandExt;
            cmd.creation_flags(0x0800_0000); // CREATE_NO_WINDOW: output comes to us instead
        }
        let mut child = cmd.spawn().map_err(|e| format!("Couldn't start {}: {e}", exe.display()))?;

        let (tx, rx) = channel();
        let mut readers = Vec::new();
        if let Some(out) = child.stdout.take() {
            readers.push(spawn_reader(out, LineKind::Out, tx.clone(), ctx.clone()));
        }
        if let Some(err) = child.stderr.take() {
            readers.push(spawn_reader(err, LineKind::Err, tx.clone(), ctx.clone()));
        }
        let child = Arc::new(Mutex::new(child));
        {
            let child = child.clone();
            let ctx = ctx.clone();
            std::thread::spawn(move || {
                let code = loop {
                    match child.lock().unwrap().try_wait() {
                        Ok(Some(status)) => break status.code(),
                        Ok(None) => {}
                        Err(_) => break None,
                    }
                    std::thread::sleep(Duration::from_millis(100));
                };
                for r in readers {
                    let _ = r.join();
                }
                let _ = tx.send(Event::Exited(code));
                ctx.request_repaint();
            });
        }

        self.lines.clear();
        self.child = Some(child);
        self.rx = Some(rx);
        self.started = Some(Instant::now());
        self.profile_id = profile_id;
        self.stop_requested = false;
        self.last_exit = None;
        self.runs += 1;
        self.last_run = Some(RunInfo {
            exe: exe.to_path_buf(),
            args: args.to_vec(),
            work_dir: work_dir.to_path_buf(),
            profile_id,
            profile_name: profile_name.to_owned(),
            mods: mods_from_args(args),
            started: SystemTime::now(),
        });

        let stamp = chrono::Local::now().format("%Y-%m-%d_%H-%M-%S");
        let safe: String = profile_name
            .chars()
            .map(|c| if c.is_alphanumeric() || c == '-' { c } else { '_' })
            .collect();
        let path = logs_dir().join(format!("{stamp}_{safe}.log"));
        self.log = std::fs::File::create(&path).ok().map(std::io::BufWriter::new);
        self.log_path = self.log.is_some().then_some(path);

        self.push(LineKind::Launcher, format!("Started {}", exe.display()));
        self.push(LineKind::Launcher, format!("Arguments: {}", display_args(args)));
        self.push(LineKind::Launcher, format!("Working folder: {}", work_dir.display()));
        Ok(())
    }

    pub fn stop(&mut self) {
        if let Some(child) = &self.child {
            self.stop_requested = true;
            let _ = child.lock().unwrap().kill();
        }
    }

    /// Drains output. Returns `Some` once, on the frame the game is seen to have exited.
    pub fn poll(&mut self) -> Option<Finished> {
        let rx = self.rx.take()?;
        let mut exited = None;
        while let Ok(ev) = rx.try_recv() {
            match ev {
                Event::Line(kind, text) => self.push(kind, text),
                Event::Exited(code) => exited = Some(code),
            }
        }
        let Some(code) = exited else {
            self.rx = Some(rx);
            return None;
        };
        let ran_for = self.elapsed();
        let exit = LastExit {
            code,
            stopped_by_user: self.stop_requested,
            ran_for,
            when: SystemTime::now(),
            log_path: self.log_path.clone(),
        };
        self.push(LineKind::Launcher, exit.summary());
        if let Some(mut log) = self.log.take() {
            let _ = log.flush();
        }
        self.last_exit = Some(exit);
        self.child = None;
        self.started = None;
        Some(Finished { profile_id: self.profile_id, seconds: ran_for.as_secs() })
    }
}

fn spawn_reader<R: Read + Send + 'static>(
    stream: R,
    kind: LineKind,
    tx: Sender<Event>,
    ctx: egui::Context,
) -> std::thread::JoinHandle<()> {
    std::thread::spawn(move || {
        let mut reader = BufReader::new(stream);
        let mut buf = Vec::new();
        loop {
            buf.clear();
            match reader.read_until(b'\n', &mut buf) {
                Ok(0) | Err(_) => break,
                Ok(_) => {
                    let text = String::from_utf8_lossy(&buf);
                    let text = strip_ansi(text.trim_end_matches(['\r', '\n']));
                    if tx.send(Event::Line(kind, text)).is_err() {
                        break;
                    }
                    ctx.request_repaint_after(Duration::from_millis(50));
                }
            }
        }
    })
}

/// Removes ANSI colour escapes so they don't show up as garbage.
fn strip_ansi(s: &str) -> String {
    let mut out = String::with_capacity(s.len());
    let mut chars = s.chars().peekable();
    while let Some(c) = chars.next() {
        if c == '\u{1b}' && chars.peek() == Some(&'[') {
            chars.next();
            for c in chars.by_ref() {
                if c.is_ascii_alphabetic() {
                    break;
                }
            }
        } else {
            out.push(c);
        }
    }
    out
}

/// Renders arguments the way they'd be typed in a shell.
pub fn display_args(args: &[String]) -> String {
    args.iter()
        .map(|a| {
            if a.is_empty() || a.contains([' ', '\t']) {
                // Same quoting Windows process creation applies to the real argument.
                format!("\"{a}\"")
            } else {
                a.clone()
            }
        })
        .collect::<Vec<_>>()
        .join(" ")
}

/// Splits a free-form "extra arguments" string, honouring double quotes.
pub fn split_args(s: &str) -> Vec<String> {
    let mut out = Vec::new();
    let mut cur = String::new();
    let mut quoted = false;
    let mut any = false;
    for c in s.chars() {
        match c {
            '"' => {
                quoted = !quoted;
                any = true;
            }
            c if c.is_whitespace() && !quoted => {
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

// ------------------------------------------------------------------ bug report

/// How many of the last output lines a bug report carries.
pub const REPORT_LINES: usize = 200;

/// Replaces the Windows user name in every path, so `C:\Users\<name>\...` reads
/// `C:\Users\<user>\...`. Works on any text: command lines, log lines, JSON-escaped paths.
/// The shared folders under `Users` (Public, Default) are left as they are.
pub fn redact_user(text: &str) -> String {
    const SHARED: [&str; 4] = ["public", "default", "default user", "all users"];
    // ASCII lower-casing keeps every byte offset, so positions carry over to `text`.
    let lower = text.to_ascii_lowercase();
    let bytes = lower.as_bytes();
    let sep = |b: u8| b == b'\\' || b == b'/';
    let ends_name = |b: u8| sep(b) || matches!(b, b'"' | b'<' | b'>' | b'|' | b':' | b'*' | b'?' | b'\r' | b'\n' | b'\t');
    let mut out = String::with_capacity(text.len());
    let mut copied = 0;
    let mut i = 0;
    while i < bytes.len() {
        let hit = sep(bytes[i]) && lower[i + 1..].starts_with("users") && bytes.get(i + 6).copied().map(sep).unwrap_or(false);
        if !hit {
            i += 1;
            continue;
        }
        let mut start = i + 6;
        while start < bytes.len() && sep(bytes[start]) {
            start += 1;
        }
        let mut end = start;
        while end < bytes.len() && !ends_name(bytes[end]) {
            end += 1;
        }
        let name = lower[start..end].trim();
        if !name.is_empty() && !SHARED.contains(&name) {
            out.push_str(&text[copied..start]);
            out.push_str("<user>");
            copied = end;
        }
        i = end.max(i + 1);
    }
    out.push_str(&text[copied..]);
    out
}

/// "Windows 11 Pro 24H2 (build 26100.2314)" from the registry's version values.
fn describe_windows(product: &str, display_version: &str, build: &str, revision: Option<u32>) -> String {
    let mut name = if product.trim().is_empty() { "Windows".to_owned() } else { product.trim().to_owned() };
    // Windows 11 still calls itself "Windows 10" in this registry value.
    if build.trim().parse::<u32>().map(|b| b >= 22000).unwrap_or(false) {
        name = name.replace("Windows 10", "Windows 11");
    }
    if !display_version.trim().is_empty() {
        name.push(' ');
        name.push_str(display_version.trim());
    }
    match (build.trim(), revision) {
        ("", _) => name,
        (b, Some(r)) => format!("{name} (build {b}.{r})"),
        (b, None) => format!("{name} (build {b})"),
    }
}

/// The Windows edition and build this is running on.
pub fn windows_version() -> String {
    #[cfg(windows)]
    {
        use winreg::enums::HKEY_LOCAL_MACHINE;
        let key = "SOFTWARE\\Microsoft\\Windows NT\\CurrentVersion";
        if let Ok(k) = winreg::RegKey::predef(HKEY_LOCAL_MACHINE).open_subkey(key) {
            let s = |name: &str| k.get_value::<String, _>(name).unwrap_or_default();
            let revision = k.get_value::<u32, _>("UBR").ok();
            return describe_windows(&s("ProductName"), &s("DisplayVersion"), &s("CurrentBuild"), revision);
        }
    }
    "Windows (version unknown)".to_owned()
}

/// One mod as a bug report lists it.
#[derive(Clone, Debug, Default)]
pub struct ReportMod {
    pub name: String,
    pub version: String,
    /// Where it was found: "Workshop", "Mods folder", ...
    pub source: String,
    pub workshop_id: Option<String>,
    pub path: String,
}

/// How the run a report describes ended.
#[derive(Clone, Debug)]
pub struct ReportExit {
    /// `LastExit::summary()`.
    pub summary: String,
    pub code: Option<i32>,
    pub ran_for: Duration,
    pub ended: String,
}

/// Everything a bug report is built from, already as plain values.
#[derive(Clone, Debug, Default)]
pub struct ReportInput {
    pub generated: String,
    pub launcher_version: String,
    pub windows: String,
    pub exe: String,
    pub exe_size: Option<u64>,
    pub exe_modified: Option<String>,
    /// The exact command line of the run.
    pub command_line: String,
    pub work_dir: String,
    pub started: String,
    pub profile: String,
    /// The mods the run used, in load order.
    pub mods: Vec<ReportMod>,
    /// `None` while the game is still running.
    pub exit: Option<ReportExit>,
    pub running_for: Option<Duration>,
    /// The output to include, oldest first (the caller passes the last [`REPORT_LINES`]).
    pub lines: Vec<ConsoleLine>,
    /// How many lines the run has printed in all.
    pub total_lines: usize,
}

fn minutes(d: Duration) -> String {
    let s = d.as_secs();
    if s >= 3600 {
        format!("{}:{:02}:{:02}", s / 3600, (s % 3600) / 60, s % 60)
    } else {
        format!("{}:{:02}", s / 60, s % 60)
    }
}

/// The plain-text bug report. The Windows user name is redacted from every path in it.
pub fn build_report(r: &ReportInput) -> String {
    let mut t = String::new();
    let mut line = |s: String| {
        t.push_str(&s);
        t.push('\n');
    };
    line("Command Center bug report".into());
    line(format!("Generated: {}", r.generated));
    line("(The Windows user name in paths is replaced with <user>.)".into());
    line(String::new());
    line(format!("Launcher: Command Center {}", r.launcher_version));
    line(format!("Windows:  {}", r.windows));
    line(String::new());
    line("Game".into());
    line(format!("  Executable: {}", r.exe));
    line(match (r.exe_size, &r.exe_modified) {
        (Some(size), Some(when)) => format!("  File: {size} bytes, modified {when}"),
        (Some(size), None) => format!("  File: {size} bytes"),
        _ => "  File: not found".to_owned(),
    });
    line(format!("  Command line: {}", r.command_line));
    line(format!("  Working folder: {}", r.work_dir));
    line(String::new());
    line(format!("Profile: {}", r.profile));
    if r.mods.is_empty() {
        line("Mods: none".into());
    } else {
        line(format!("Mods ({}, in load order; the first wins):", r.mods.len()));
        for (i, m) in r.mods.iter().enumerate() {
            let version = if m.version.trim().is_empty() { "no version".to_owned() } else { format!("version {}", m.version.trim()) };
            let workshop = match &m.workshop_id {
                Some(id) => format!("Workshop id {id}"),
                None => "no Workshop id".to_owned(),
            };
            line(format!("  {}. {} | {} | {} | {}", i + 1, m.name, version, m.source, workshop));
            line(format!("     {}", m.path));
        }
    }
    line(String::new());
    line("Last run".into());
    line(format!("  Started: {}", r.started));
    match (&r.exit, r.running_for) {
        (Some(e), _) => {
            line(format!("  Result: {}", e.summary));
            line(match e.code {
                Some(c) => format!("  Exit code: {c} (0x{:08X})", c as u32),
                None => "  Exit code: none".to_owned(),
            });
            line(format!("  Ran for: {}", minutes(e.ran_for)));
            line(format!("  Ended: {}", e.ended));
        }
        (None, Some(d)) => line(format!("  Result: still running after {}", minutes(d))),
        (None, None) => line("  Result: unknown".into()),
    }
    line(String::new());
    let shown = r.lines.len();
    line(if shown == 0 {
        "Output: none".to_owned()
    } else if shown < r.total_lines {
        format!("Output (last {shown} of {} lines; ! = error stream, # = launcher):", r.total_lines)
    } else {
        format!("Output ({shown} lines; ! = error stream, # = launcher):")
    });
    for l in &r.lines {
        let tag = match l.kind {
            LineKind::Out => ' ',
            LineKind::Err => '!',
            LineKind::Launcher => '#',
        };
        line(format!("[{:9.3}]{tag} {}", l.at, l.text));
    }
    redact_user(&t)
}

#[cfg(test)]
mod tests {
    use super::*;

    #[test]
    fn user_names_are_redacted_from_paths() {
        for (input, expect) in [
            (r"C:\Users\Gabriel\AppData\Local\alamo\Saves", r"C:\Users\<user>\AppData\Local\alamo\Saves"),
            (r"c:\users\gabriel\x", r"c:\users\<user>\x"),
            ("C:/Users/Jane Doe/Documents/mod", "C:/Users/<user>/Documents/mod"),
            (r"D:\Users\bob", r"D:\Users\<user>"),
            (r#"MODPATH=C:\Users\bob\Mods\A "--saves-dir" "C:\Users\bob\My Saves""#, r#"MODPATH=C:\Users\<user>\Mods\A "--saves-dir" "C:\Users\<user>\My Saves""#),
            // JSON-escaped and extended-length forms.
            (r#"{"path":"C:\\Users\\bob\\x"}"#, r#"{"path":"C:\\Users\\<user>\\x"}"#),
            (r"\\?\C:\Users\bob\x", r"\\?\C:\Users\<user>\x"),
            ("first C:\\Users\\ann\\a\nsecond C:\\Users\\ann\nthird", "first C:\\Users\\<user>\\a\nsecond C:\\Users\\<user>\nthird"),
            // Non-ASCII names.
            (r"C:\Users\Jürgen Müller\x", r"C:\Users\<user>\x"),
            // Nothing to redact.
            (r"C:\Users\Public\Documents", r"C:\Users\Public\Documents"),
            (r"C:\Users\Default\NTUSER.DAT", r"C:\Users\Default\NTUSER.DAT"),
            (r"C:\Users\", r"C:\Users\"),
            (r"C:\Users", r"C:\Users"),
            (r"E:\Games\PowerUsers\bob\x", r"E:\Games\PowerUsers\bob\x"),
            (r"C:\Users\<user>\x", r"C:\Users\<user>\x"),
            ("", ""),
        ] {
            assert_eq!(redact_user(input), expect, "input: {input}");
        }
        // Idempotent.
        let once = redact_user(r"C:\Users\bob\a C:\Users\bob\b");
        assert_eq!(redact_user(&once), once);
    }

    #[test]
    fn windows_version_text() {
        assert_eq!(
            describe_windows("Windows 10 Pro", "24H2", "26100", Some(2314)),
            "Windows 11 Pro 24H2 (build 26100.2314)"
        );
        assert_eq!(describe_windows("Windows 10 Home", "22H2", "19045", None), "Windows 10 Home 22H2 (build 19045)");
        assert_eq!(describe_windows("", "", "", None), "Windows");
        assert!(windows_version().starts_with("Windows"));
    }

    #[test]
    fn mods_are_read_back_from_the_arguments() {
        let args: Vec<String> =
            ["--game", r"C:\FoC", r"MODPATH=C:\Mods\A B", "MODPATH=D:\\ws\\123", "--no-intro", "FOO=bar", "MODPATH="]
                .iter()
                .map(|s| s.to_string())
                .collect();
        assert_eq!(mods_from_args(&args), vec![PathBuf::from(r"C:\Mods\A B"), PathBuf::from("D:\\ws\\123")]);
        let run = RunInfo {
            exe: PathBuf::from(r"C:\Program Files\alamo\alamo_game.exe"),
            args,
            work_dir: PathBuf::from(r"C:\Program Files\alamo"),
            profile_id: 1,
            profile_name: "P".into(),
            mods: Vec::new(),
            started: SystemTime::UNIX_EPOCH,
        };
        assert_eq!(
            run.command_line(),
            r#""C:\Program Files\alamo\alamo_game.exe" --game C:\FoC "MODPATH=C:\Mods\A B" MODPATH=D:\ws\123 --no-intro FOO=bar MODPATH="#
        );
    }

    fn sample_report() -> ReportInput {
        let l = |kind, at, text: &str| ConsoleLine { kind, text: text.to_owned(), at };
        ReportInput {
            generated: "12 Nov 2026, 20:30".into(),
            launcher_version: "0.1.0".into(),
            windows: "Windows 11 Pro 24H2 (build 26100.2314)".into(),
            exe: r"C:\Users\gabriel\Games\alamo\alamo_game.exe".into(),
            exe_size: Some(12_345_678),
            exe_modified: Some("10 Nov 2026, 18:22".into()),
            command_line: r#"C:\Users\gabriel\Games\alamo\alamo_game.exe --game "D:\Steam\FoC" MODPATH=D:\ws\1125571106 "MODPATH=C:\Users\gabriel\My Mods\Patch" --no-intro"#.into(),
            work_dir: r"C:\Users\gabriel\Games\alamo".into(),
            started: "12 Nov 2026, 20:28".into(),
            profile: "Thrawn's Revenge".into(),
            mods: vec![
                ReportMod {
                    name: "Thrawn's Revenge".into(),
                    version: "3.4".into(),
                    source: "Workshop".into(),
                    workshop_id: Some("1125571106".into()),
                    path: r"D:\ws\1125571106".into(),
                },
                ReportMod {
                    name: "Patch".into(),
                    version: String::new(),
                    source: "Extra folder".into(),
                    workshop_id: None,
                    path: r"C:\Users\gabriel\My Mods\Patch".into(),
                },
            ],
            exit: Some(ReportExit {
                summary: "Crashed: access violation (0xC0000005).".into(),
                code: Some(0xC0000005u32 as i32),
                ran_for: Duration::from_secs(75),
                ended: "12 Nov 2026, 20:29".into(),
            }),
            running_for: None,
            lines: vec![
                l(LineKind::Launcher, 0.0, r"Started C:\Users\gabriel\Games\alamo\alamo_game.exe"),
                l(LineKind::Out, 1.5, "loading mods"),
                l(LineKind::Err, 74.25, r"error: can't open C:\Users\gabriel\My Mods\Patch\Data\x.xml"),
            ],
            total_lines: 950,
        }
    }

    #[test]
    fn the_bug_report_has_every_section() {
        let text = build_report(&sample_report());
        let expect = r#"Command Center bug report
Generated: 12 Nov 2026, 20:30
(The Windows user name in paths is replaced with <user>.)

Launcher: Command Center 0.1.0
Windows:  Windows 11 Pro 24H2 (build 26100.2314)

Game
  Executable: C:\Users\<user>\Games\alamo\alamo_game.exe
  File: 12345678 bytes, modified 10 Nov 2026, 18:22
  Command line: C:\Users\<user>\Games\alamo\alamo_game.exe --game "D:\Steam\FoC" MODPATH=D:\ws\1125571106 "MODPATH=C:\Users\<user>\My Mods\Patch" --no-intro
  Working folder: C:\Users\<user>\Games\alamo

Profile: Thrawn's Revenge
Mods (2, in load order; the first wins):
  1. Thrawn's Revenge | version 3.4 | Workshop | Workshop id 1125571106
     D:\ws\1125571106
  2. Patch | no version | Extra folder | no Workshop id
     C:\Users\<user>\My Mods\Patch

Last run
  Started: 12 Nov 2026, 20:28
  Result: Crashed: access violation (0xC0000005).
  Exit code: -1073741819 (0xC0000005)
  Ran for: 1:15
  Ended: 12 Nov 2026, 20:29

Output (last 3 of 950 lines; ! = error stream, # = launcher):
[    0.000]# Started C:\Users\<user>\Games\alamo\alamo_game.exe
[    1.500]  loading mods
[   74.250]! error: can't open C:\Users\<user>\My Mods\Patch\Data\x.xml
"#;
        assert_eq!(text, expect);
        assert!(!text.to_lowercase().contains("gabriel"));
    }

    #[test]
    fn the_bug_report_copes_with_missing_pieces() {
        let mut r = sample_report();
        r.mods.clear();
        r.exit = None;
        r.running_for = Some(Duration::from_secs(3723));
        r.exe_size = None;
        r.exe_modified = None;
        r.lines.clear();
        r.total_lines = 0;
        let text = build_report(&r);
        assert!(text.contains("\nMods: none\n"), "{text}");
        assert!(text.contains("\n  Result: still running after 1:02:03\n"), "{text}");
        assert!(text.contains("\n  File: not found\n"), "{text}");
        assert!(text.ends_with("Output: none\n"), "{text}");

        r.exit = Some(ReportExit { summary: "Ended without an exit code.".into(), code: None, ran_for: Duration::ZERO, ended: "now".into() });
        r.lines = sample_report().lines;
        r.total_lines = 3;
        let text = build_report(&r);
        assert!(text.contains("\n  Exit code: none\n"), "{text}");
        assert!(text.contains("\nOutput (3 lines; ! = error stream, # = launcher):\n"), "{text}");
    }
}

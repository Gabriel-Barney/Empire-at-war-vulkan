//! Command Center — launcher and control panel for the Empire at War Vulkan port.
#![cfg_attr(not(debug_assertions), windows_subsystem = "windows")]

mod app;
mod bisect;
mod config;
mod conflicts;
mod diagnostics;
mod galaxy;
mod launch;
mod layout;
mod meg;
mod mods;
mod notify;
mod pages;
mod palette;
mod profile_io;
mod runner;
mod saves;
mod shortcut;
mod steam;
mod theme;
mod unitdb;
mod video;
mod workshop;
mod xml;

use std::sync::Arc;

/// What the command line asked for.
#[derive(Clone, Debug, Default, PartialEq, Eq)]
pub struct Cli {
    /// `--profile <name or id>`: make this the active profile.
    pub profile: Option<String>,
    /// `--launch`: start the game as soon as the window is up.
    pub launch: bool,
}

/// Reads `--profile <name>` (or `--profile=<name>`) and `--launch`. Anything else is left
/// alone, so an old shortcut with a flag this build doesn't know still opens the launcher.
pub fn parse_cli(args: &[String]) -> Cli {
    let mut cli = Cli::default();
    let mut args = args.iter().peekable();
    while let Some(arg) = args.next() {
        if arg == "--launch" {
            cli.launch = true;
        } else if arg == "--profile" {
            // `--profile --launch` has no name; don't swallow the flag as one.
            if args.peek().is_some_and(|next| *next != "--launch") {
                cli.profile = args.next().cloned();
            }
        } else if let Some(name) = arg.strip_prefix("--profile=") {
            cli.profile = Some(name.to_owned());
        }
    }
    cli.profile = cli.profile.map(|p| p.trim().to_owned()).filter(|p| !p.is_empty());
    cli
}

/// The profile `--profile` means: its name, ignoring case, or else its numeric id.
pub fn find_profile(profiles: &[config::Profile], wanted: &str) -> Option<u64> {
    let wanted = wanted.trim();
    let by_name = |exact: bool| {
        profiles.iter().find(|p| {
            let name = p.name.trim();
            if exact {
                name == wanted
            } else {
                name.to_lowercase() == wanted.to_lowercase()
            }
        })
    };
    by_name(true)
        .or_else(|| by_name(false))
        .or_else(|| wanted.parse::<u64>().ok().and_then(|id| profiles.iter().find(|p| p.id == id)))
        .map(|p| p.id)
}

fn main() -> eframe::Result<()> {
    let args: Vec<String> = std::env::args_os().skip(1).map(|a| a.to_string_lossy().into_owned()).collect();
    let cli = parse_cli(&args);
    let cfg = config::Config::load();
    let use_gpu = cfg.settings.gpu_rendering;

    let mut native = eframe::NativeOptions {
        viewport: egui::ViewportBuilder::default()
            .with_title("Command Center")
            .with_inner_size([1320.0, 840.0])
            .with_min_inner_size(app::MIN_WINDOW)
            .with_icon(app::app_icon()),
        renderer: eframe::Renderer::Wgpu,
        ..Default::default()
    };

    // "GPU drawing" setting: with it off we pick a CPU adapter (WARP on Windows) so the
    // launcher never touches the graphics card the game is about to use.
    let mut setup = egui_wgpu::WgpuSetupCreateNew::default();
    setup.power_preference = wgpu::PowerPreference::LowPower;
    setup.native_adapter_selector = Some(Arc::new(move |adapters, surface| {
        let usable: Vec<&wgpu::Adapter> = adapters
            .iter()
            .filter(|a| surface.map(|s| a.is_surface_supported(s)).unwrap_or(true))
            .collect();
        let is_cpu = |a: &&wgpu::Adapter| a.get_info().device_type == wgpu::DeviceType::Cpu;
        let rank = |a: &&wgpu::Adapter| match a.get_info().device_type {
            wgpu::DeviceType::IntegratedGpu => 0,
            wgpu::DeviceType::DiscreteGpu => 1,
            wgpu::DeviceType::VirtualGpu => 2,
            wgpu::DeviceType::Other => 3,
            wgpu::DeviceType::Cpu => 4,
        };
        let pick = if use_gpu {
            usable.iter().min_by_key(|a| rank(a)).copied()
        } else {
            usable.iter().find(|a| is_cpu(a)).copied().or_else(|| usable.first().copied())
        };
        pick.cloned().ok_or_else(|| "No usable graphics adapter found".to_owned())
    }));
    native.wgpu_options.wgpu_setup = egui_wgpu::WgpuSetup::CreateNew(setup);

    eframe::run_native(
        "Command Center",
        native,
        Box::new(move |cc| Ok(Box::new(app::App::new(cc, cfg, cli)))),
    )
}

#[cfg(test)]
mod tests {
    use super::*;

    fn cli(args: &[&str]) -> Cli {
        parse_cli(&args.iter().map(|a| a.to_string()).collect::<Vec<_>>())
    }

    #[test]
    fn cli_reads_profile_and_launch() {
        assert_eq!(cli(&[]), Cli::default());
        assert_eq!(cli(&["--launch"]), Cli { profile: None, launch: true });
        assert_eq!(
            cli(&["--profile", "Thrawn's Revenge", "--launch"]),
            Cli { profile: Some("Thrawn's Revenge".into()), launch: true }
        );
        // Order doesn't matter, and the = form works too.
        assert_eq!(cli(&["--launch", "--profile=Vanilla"]), Cli { profile: Some("Vanilla".into()), launch: true });
        assert_eq!(cli(&["--profile", " 3 "]), Cli { profile: Some("3".into()), launch: false });
        // A missing or empty name is no profile, and never eats --launch.
        assert_eq!(cli(&["--profile"]), Cli::default());
        assert_eq!(cli(&["--profile", "--launch"]), Cli { profile: None, launch: true });
        assert_eq!(cli(&["--profile=", "--launch"]), Cli { profile: None, launch: true });
        assert_eq!(cli(&["--profile", "  "]), Cli::default());
        // Unknown arguments are ignored; the last --profile wins.
        assert_eq!(
            cli(&["--verbose", "--profile", "A", "x", "--profile", "B"]),
            Cli { profile: Some("B".into()), launch: false }
        );
        // The retired Steam helper's argument (old shortcuts, scripts) opens the launcher.
        assert_eq!(cli(&["--steam-workshop-helper"]), Cli::default());
    }

    #[test]
    fn cli_finds_the_profile() {
        let profiles = vec![
            config::Profile::new(1, "Vanilla"),
            config::Profile::new(2, "Thrawn's Revenge"),
            config::Profile::new(7, "thrawn's revenge"),
            config::Profile::new(9, "2"),
            config::Profile::new(12, " Padded "),
        ];
        assert_eq!(find_profile(&profiles, "Vanilla"), Some(1));
        assert_eq!(find_profile(&profiles, "vANILLA"), Some(1));
        assert_eq!(find_profile(&profiles, "  vanilla "), Some(1));
        // An exact match beats one that differs in case.
        assert_eq!(find_profile(&profiles, "thrawn's revenge"), Some(7));
        assert_eq!(find_profile(&profiles, "Thrawn's Revenge"), Some(2));
        assert_eq!(find_profile(&profiles, "THRAWN'S REVENGE"), Some(2));
        // A name wins over an id; an id works when no name matches.
        assert_eq!(find_profile(&profiles, "2"), Some(9));
        assert_eq!(find_profile(&profiles, "7"), Some(7));
        assert_eq!(find_profile(&profiles, "12"), Some(12));
        assert_eq!(find_profile(&profiles, "padded"), Some(12));
        assert_eq!(find_profile(&profiles, "Nope"), None);
        assert_eq!(find_profile(&profiles, "3"), None);
        assert_eq!(find_profile(&profiles, ""), None);
    }
}

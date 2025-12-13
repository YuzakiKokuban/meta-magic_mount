use std::{
    ffi::{CString, c_char, c_int},
    fs::{self, OpenOptions, read_dir},
    io,
    os::fd::{AsRawFd, RawFd},
    path::Path,
    sync::OnceLock,
};

use anyhow::Result;
use rustix::path::Arg;

use crate::defs::{DISABLE_FILE_NAME, REMOVE_FILE_NAME, SKIP_MOUNT_FILE_NAME};

const K: u32 = b'K' as u32;
const KSU_INSTALL_MAGIC1: u32 = 0xDEAD_BEEF;
#[cfg(target_env = "gnu")]
const KSU_IOCTL_ADD_TRY_UMOUNT: u64 = libc::_IOW::<()>(K, 18);
#[cfg(not(target_env = "gnu"))]
const KSU_IOCTL_ADD_TRY_UMOUNT: i32 = libc::_IOW::<()>(K, 18);
const KSU_INSTALL_MAGIC2: u32 = 0xCAFE_BABE;
static DRIVER_FD: OnceLock<RawFd> = OnceLock::new();

const HYMO_IOC_MAGIC: u32 = 0xE0;
const HYMO_IOCTL_HIDE: u64 = ioctl_cmd_write(3, std::mem::size_of::<HymoHide>());
const HYMO_DEV: &[&str] = &["/dev/hymo_ctl", "/proc/hymo_ctl"];

#[repr(C)]
struct HymoHide {
    src: *const c_char,
    target: *const c_char,
    r#type: c_int,
}

#[repr(C)]
struct KsuAddTryUmount {
    arg: u64,
    flags: u32,
    mode: u8,
}

const fn ioctl_cmd_write(nr: u32, size: usize) -> u64 {
    let size = size as u64;
    (1u32 << 30) as u64 | (size << 16) | ((HYMO_IOC_MAGIC as u64) << 8) | nr as u64
}

fn find_node() -> Option<RawFd> {
    for i in HYMO_DEV {
        if let Ok(dev) = OpenOptions::new().read(true).write(true).open(i) {
            return Some(dev.as_raw_fd());
        }
    }

    None
}
fn send_hide_hymofs<P>(target: P) -> Result<()>
where
    P: AsRef<Path>,
{
    let fd = find_node();

    if fd.is_none() {
        return Ok(());
    }
    let fd = fd.unwrap();

    let path = CString::new(target.as_ref().as_str()?)?;
    let cmd = HymoHide {
        src: path.as_ptr(),
        target: std::ptr::null(),
        r#type: 0,
    };

    let ret = unsafe {
        #[cfg(not(target_env = "gnu"))]
        {
            libc::ioctl(fd, HYMO_IOCTL_HIDE as i32, &cmd)
        }
        #[cfg(target_env = "gnu")]
        {
            libc::ioctl(fd, HYMO_IOCTL_HIDE, &cmd)
        }
    };
    if ret < 0 {
        log::error!(
            "umount {} failed: {}",
            target.as_ref().display(),
            io::Error::last_os_error()
        );

        return Ok(());
    }

    log::info!("umount {} successful!", target.as_ref().display());
    Ok(())
}

fn send_kernel_umount<P>(target: P) -> Result<()>
where
    P: AsRef<Path>,
{
    let path = CString::new(target.as_ref().as_str()?)?;
    let cmd = KsuAddTryUmount {
        arg: path.as_ptr() as u64,
        flags: 2,
        mode: 1,
    };

    let fd = *DRIVER_FD.get_or_init(|| {
        let mut fd = -1;
        unsafe {
            libc::syscall(
                libc::SYS_reboot,
                KSU_INSTALL_MAGIC1,
                KSU_INSTALL_MAGIC2,
                0,
                &mut fd,
            );
        };
        fd
    });

    unsafe {
        #[cfg(target_env = "gnu")]
        let ret = libc::ioctl(fd as libc::c_int, KSU_IOCTL_ADD_TRY_UMOUNT, &cmd);

        #[cfg(not(target_env = "gnu"))]
        let ret = libc::ioctl(fd as libc::c_int, KSU_IOCTL_ADD_TRY_UMOUNT, &cmd);

        if ret < 0 {
            log::error!(
                "umount {} failed: {}",
                target.as_ref().display(),
                io::Error::last_os_error()
            );

            return Ok(());
        }

        log::info!("umount {} successful!", target.as_ref().display());
    };
    Ok(())
}

pub fn send_unmountable<P>(target: P) -> Result<()>
where
    P: AsRef<Path>,
{
    for entry in read_dir("/data/adb/modules")?.flatten() {
        let path = entry.path();

        if !path.is_dir() {
            continue;
        }

        if !path.join("module.prop").exists() {
            continue;
        }

        let disabled =
            path.join(DISABLE_FILE_NAME).exists() || path.join(REMOVE_FILE_NAME).exists();
        let skip = path.join(SKIP_MOUNT_FILE_NAME).exists();
        if disabled || skip {
            continue;
        }

        if let Some(name) = path.file_name()
            && name.to_string_lossy().to_string().contains("zygisksu")
            && fs::read_to_string("/data/adb/zygisksu/denylist_enforce")?.trim() == "0"
        {
            log::warn!("zn was detected, and try_umount was cancelled.");
            return Ok(());
        }
    }

    for i in HYMO_DEV {
        if fs::exists(i)? {
            send_hide_hymofs(target.as_ref())?;
            return Ok(());
        }
    }

    send_kernel_umount(target.as_ref())?;
    Ok(())
}

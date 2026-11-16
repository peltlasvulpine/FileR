# FileR

File manager for the TI-84 Plus CE (or similar) with USB support!
This is mainly to be used **with** a USB connected. For more information on how to set this up, [check below](#how-to-format-a-usb-device-for-the-calculator).

> Please have common sense when using this program, don't do something that will freeze the program, or break it, like trying to move a folder into itself (although this is already handled to NOT let you do this).
> If you do find glitches, please report them to me via [issues on GitHub](https://github.com/peltlasvulpine/FileR/issues)!

Has been tested on CEmu and on actual hardware (TI-84 Plus CE) with OS 5.4.0.0034 .

This wasn't made to be a shell, but use it as one if you want to.

### Status

Targets the TI-84 Plus CE with OS 5.4.0.0034

Main app is complete but new features are bound to come.
Some things are a little unstable still, fixes will be applied in the future.

### How to build

1. Make sure you have the [CE C toolchain](https://github.com/CE-Programming/toolchain) installed and added to your path.
2. Ensure you've cloned this GitHub repository.
3. Run `make -j install-kit` from this repository's root. (Or, if you've already built this and want to do a fresh build, run `make clean-install && make -j install-kit` from repo root.)
4. Copy all `.8xp` & `.8xv` files to your calculator via external tools.


## Current capabilities & checklist

- [x] Read files
- [x] Move files
- [x] Rename files
- [x] Copy files
- [x] Write to files
- [x] List files
- [x] Filter through files
- [x] Nice(ish) UI
- [ ] Clock (why didn't I think of this sooner???)
- [x] Battery display
- [x] Charging handling(ish)
- [x] Icon support (colors are a little messed up & sometimes doesnt find the icon)
- [ ] Program execution
- [x] Showing apps
- [ ] App execution
- [x] USB support
- [x] USB partition support
- [x] USB partition names
- [x] Copy from USB to calculator
- [x] Copy from calculator to USB

### Supported File types

These are the currently supported file types (to open)

- `.txt`
- `.html`
- `.htm`
- `.bmp` (uncompressed)
- `.gif`

## Usage / Keybinds

### General

| Key | Action |
|---|---|
| Up | Move selection up (Move up 10 (Alpha)) |
| Down | Move selection down (Move down 10 (Alpha))|
| 2nd | Toggle Shift |
| Alpha | Toggle Alpha / Alpha Lock (Shift) |
| Y= | Search |
| Graph | Change filters |
| Clear | Clear actions |
| Mode | Open customization menu / Exit FileR (Shift) |
| Del | Deletes selection |

> Deleting `FILERCFG` resets colors

#### On calculator root

| Key | Action |
|---|---|
| Window | Open calculator variable info |
| Trace | Export current selection |
| Enter | Open USB / Open USB partitions (If more than one) |

> Pressing Window while selecting USB will give an option to unmount by pressing 2nd
> Unmounting isn't required to remove the USB, & when exiting the program, the USB device will not auto-unmount

#### Inside `/usb`

| Key | Action |
|---|---|
| Window | Open `Files` Submenu |
| Zoom | Open `Actions` Submenu |
| Trace | Mark / Select an entry |
| Vars | Edit a text file |
| Enter | Enter actions |

#### Text Viewer

| Key | Action |
|---|---|
| Up | Go one line up |
| Down | Go one line down |
| Left | Go one page up |
| Right | Go one page down |
| Graph | Jump to |
| Zoom | Toggles between percentage & line jump |
| Enter | Close the text viewer |
| Clear | Close the text viewer |

#### Text Editor
> Exiting the text editor with 2nd + Mode (Quit) will generate an autosave in that text file's directory
##### Keybinds
| Key | Action |
|---|---|
| Up | Go one line up / Go one page up (Alpha) |
| Down | Go one line down / Go one page down (Alpha) |
| Left | Move cursor one character left |
| Right | Move cursor one character right |
| Trace | Jump to |
| Zoom | Toggles between percentage & line jump |
| Clear | Close the text editor |

##### Special character entry
| Key | Modifier | Character |
|---|---|---|
| Left | Alpha | `<` |
| Right | Alpha | `>` |
| `.` | Shift | `;` |
| `-` |  | `~` |


#### Variable info viewer
| Key | Action |
|---|---|
| Y= | Rename |
| Window | Close calculator variable info |
| Zoom | Archive / Unarchive |
| Trace | Lock / Unlock |
| Graph | Hide / Show |

#### Actions

##### Enter
| Context | Action |
|---|---|
| On a text file | Open / Close the text viewer |
| On an image file | Open / Close the image viewer |
| On a GIF file | Open / Close the GIF viewer |

> While viewing a GIF, you should hold down Enter / Clear / Del if you want to exit.

##### Clear
| Context | Action |
|---|---|
| In calculator root | Exit FileR |
| In a directory | Go down one directory |
| While moving selections | Aborts action |
| While copying selections | Aborts action |
| On a text file | Close the text viewer |
| On an image file | Close the image viewer |
| On a GIF file | Close the GIF viewer |

> Aborting an action is really hit-and-miss, you might have to spam Clear sometimes.
> While viewing a GIF, you should hold down Enter / Clear / Del if you want to exit.

#### Submenus

##### `Files`
- New Folder
- Delete
- New File

##### `Actions`
- Rename
- Move here
- Copy here
- Edit
- Hide
- Send to calc

#### Customization menu

Each entry can be changed by either:
  - Pressing left / right to change the value by -1 / +1
  - Pressing the number pad to write the color directly

Press Enter to save, press Mode / Clear to cancel

#### Filter meanings

| Filter | Meaning |
|---|---|
| `Type` | Type of variable (Program, AppVar, etc.) |
| `Store` | Where the file is stored (Archive / RAM) |
| `Show` | Type of entry to show (Folder / File) |
| `Ext` | Extension type (`.txt`, `.html`, `.bmp`, etc.) |
| `Sort` | How to sort (`A-Z` / `Z-A`)

### How to format a USB device for the calculator

> [!CAUTION]
> **BACKUP DATA ON SAID USB DEVICE, AS THIS *CAN* & MOST LIKELY *WILL* REMOVE <u>ALL</u> DATA ON SAID USB DEVICE**

Please note that this only works with drives of **32 GB or less**

To connect a USB device, use a mini USB adapter with OTG support.

If a USB device takes some time to connect, it is usually because:
1. It's the first time the USB device has been connected.
2. Contents on the USB device have been changed. (Yes, this usually also happens if you edit files on-calculator)
3. The calculator's RAM has been reset.

#### Windows (11/10)

1. Go to your search bar & search up "*Create and*", Windows *should* auto-complete it to "*Create and format hard disk partitions*", when that is shown, open it.
2. In the bottom half of the *Create and format hard disk partitions* window, locate the USB to format & right-click its respective box on the left.
  - Check the menu
    1. If it says "*Convert to GPT Disk*", move to #3
    2. If it says "*Convert to MBR Disk*", select it. This will change your USB from GPT to MBR. (This is the part that wipes the USB)
      <sub>If this option is grayed out, delete all partitions on the USB, then try again.</sub>
3. Right-click the unallocated space & select "*New Simple Volume*".
4. Follow the wizard prompts until you reach the "*Format Partition*" window.
5. Change the *File system* dropdown to FAT32.
6. Check the box "*Perform a quick format*".
7. Select *Next*, then *Finish*.

#### Linux

This varies by distro, so if yours isn't mentioned, check for your equivalent online, although it should more or less resemble this tutorial

1. Install GParted.
  - Debian based
```bash
sudo apt update && sudo apt install gparted
```
  - Arch based
```bash
sudo pacman -S gparted
```
  - Red Hat Enterprise Linux / Fedora based
```bash
sudo dnf install gparted
```
2. Open GParted & select the USB to format in the top-left dropdown. (`/dev/sdX`)
  <sub>If needed, refresh GParted with `Ctrl+R`</sub>
3. Select "*Device Information*" under "*View*" in the menu bar.
  - Check "*Partition table*"
    1. If it says "*msdos*" or "*MBR*", then continue to #6
    2. If it doesn't say "*msdos*" or "*MBR*", then continue to #4
4. Unmount all partitions.
5. Select "*Create Partition Table...*" under "*Device*"&set the *Partition Table Type* dropdown to "*msdos*" or "*MBR*" (This is the part that wipes the USB)
6. After the operation finishes, right-click the unallocated space & select "*New*"
7. Change *File system* to FAT32.
8. Edit other settings to your liking.
9. Select "*Add*" & confirm the operation.

#### macOS

Sorry, I don't personally use macOS, just check for a tutorial online.

### List of external tools

Here's a list of software I've used to transfer stuff to my calculator
- [TI Connect™ CE](https://education.ti.com/en/products/computer-software/ti-connect-ce-sw)
- [TiLP](http://lpg.ticalc.org/prj_tilp/)
- [WebTILP](https://tiplanet.org/scripts/webtilibs/webtilp.html)

## Screenshots

I'll update this with screenshots later

---

#### Credits & Third-Party Code

FileR incorporates certain code and ideas from the following open-source projects:

- [**CEaShell**](https://github.com/RoccoLoxPrograms/CEaShell)
  - Authors: RoccoLox Programs and TIny_Hacker
  - License: GNU General Public License v3.0 (GPL-3.0)

- [**Cesium**](https://github.com/mateoconlechuga/cesium)
  - Author: Matt "MateoConLechuga" Waltz
  - License: BSD 3-Clause

Original copyright notices and applicable license terms are retained.
See the respective repositories for their full license terms.

---

#### Disclaimer

> *FileR is a community-made project and is not affiliated with, endorsed by, or connected to Texas Instruments. TI-84 Plus CE and TI Connect™ CE are trademarks of Texas Instruments.*

---

#### License

FileR is licensed under the **GNU General Public License v3.0 (GPL-3.0)**.

You are free to use, modify, and redistribute this software, provided that all redistributed derivative works remain licensed under GPL-3.0 and retain the appropriate copyright and license notices.

See [`LICENSE`](LICENSE) for the full license text.

# VitaShell (with LocalSend support)

This is a modified version of the renowned [VitaShell](https://github.com/TheOfficialFloW/VitaShell) file manager for the PlayStation Vita, adding full support for the [LocalSend](https://localsend.org/) protocol.

It allows you to seamlessly and wirelessly transfer files between your PS Vita and any other device running LocalSend (Android, iOS, Windows, macOS, Linux) without needing to configure FTP or use a USB cable.

## Features
- **Send & Receive:** Fully supports both sending files from your Vita and receiving files to your Vita.
- **Cross-Platform:** Works out-of-the-box with the official LocalSend apps on all major platforms.
- **High Speed:** Optimized networking stack utilizing non-blocking sockets and 2MB buffers, capable of pushing the PS Vita's 2.4GHz WiFi hardware to its absolute limits (~2-3 MB/s).
- **Auto-Discovery:** Automatically detects other LocalSend devices on your local network.
- **Multi-File Support:** Batch send or receive hundreds of files at once with accurate total-progress tracking.
- **Familiar UI:** Built directly into VitaShell, providing a familiar and powerful file manager experience.

## Installation
1. Go to the [Releases](../../releases) tab on GitHub and download `VitaShell.vpk`.
2. Transfer the `.vpk` to your PS Vita via FTP, USB, or VitaDeploy.
3. Install the `.vpk` using your existing VitaShell installation.

## How to Use
### To Send Files from Vita:
1. Open VitaShell.
2. Navigate to the files or folders you want to send.
3. Press **Square** to mark multiple files, or press **Triangle** to open the context menu.
4. Select **LocalSend**.
5. A dialog will appear searching for LocalSend devices on your WiFi network. Select the target device using the D-Pad and press **Cross**.
6. Accept the transfer on the receiving device.

### To Receive Files to Vita:
1. Open VitaShell on your Vita.
2. Open LocalSend on your phone/PC and select the files you want to send.
3. Your Vita will automatically appear in the list of nearby devices (as `VITA`). Tap it.
4. On your Vita, a prompt will appear asking you to accept the incoming files.
5. Files are automatically saved to your current directory in VitaShell!

## Building from Source
Prerequisites:
- [VitaSDK](https://vitasdk.org/) installed and configured.
- `cmake` and `make`.

Run the build script:
```bash
./setup_and_build.sh
```
The compiled `VitaShell.vpk` will be placed in the `build/` directory.

## Credits
- **[TheFlow](https://github.com/TheOfficialFloW)**: For the original [VitaShell](https://github.com/TheOfficialFloW/VitaShell) codebase, UI framework, and system utilities. This project is a direct modification of their incredible work.
- **[LocalSend](https://localsend.org/)**: For the open-source, stateless, cross-platform file transfer protocol.

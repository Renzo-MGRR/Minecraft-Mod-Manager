# An open source C++ Minecraft Mod Manager
This is an attempt to create a good open source Minecraft Mod Manager in C++ with the ImGui Standalone template by adamhlt. It mostly uses basic C++ libraries and the Windows API besides ImGui. Also uses libcurl to download stuff and bit7z to extract/list files (Use vcpkg to install libcurl).
Supports modded servers with UI implementation for them.
This mod manager makes all modded Minecraft profiles have their own directory for optimization and better organization.
The official Minecraft Launcher is strongly recommended to ensure proper functioning of this program. (launcher_profiles.json must be present in .minecraft)

## Full list of features:
- Installing CurseForge and Modrinth Modpacks, automatically downloading its files.
- Automatic download of latest Fabric Installer
- Multiple modded profiles
- Adding multiple mods or resource packs at the same time, as folder or per file
- Reloading profiles and servers with a button, rather than restarting the program
- Opening the Modrinth mod page (buggy cause of Modrinth page, not a fault of this program) and the CurseForge mod page of the current mod loader and version
- Enabling and disabling and removing mods and resource packs
- Auto-identifying mod and resource pack titles
- Installing modded servers and accepting the EULA automatically
- Removing and renaming profiles and servers
- Managing server options
- And more...
### To Do:
- Optimize the code (it's a mess)
- Check mod and modpack compatibility with current Minecraft version
- Bug and unintended behavior fixes
- Thread all of the downloads
- More testing
- And more...

# Where are my saves/screenshots/resourcepacks/mods/shaders???
- They're still on "%appdata%\.minecraft". What happens is that this program creates a separate folder for each profile, and the game then only reads from that folder. You can copy manually from that folder, I will later add an option to copy them from the program.

I'm really looking for help. If you wanna help on this project, contact me at Discord: ryxden

# Full-Screen Wrapper for DLSS5

### A demo app that applies DLSS5 to the entire screen (even the desktop) with full model controls.

Just a single `exe` (signed with a trusted certificate) written in C++ with zero third-party dependencies.

<br>

> [!NOTE]
> This is an UNOFFICIAL project, not ssociated with Nvidia.

# App Screenshot
<p align="center">
<img width="750" alt="Full-Screen Wrapper for DLSS5 — Main app screenshot" src="https://github.com/user-attachments/assets/e0d18450-cc59-4382-9990-d79043a2ed48" />
</p>

## How to Download and Use
1. Download the latest version of the `exe`. (Direct link [here](https://github.com/ThioJoe/Full-Screen-DLSS5-Wrapper/releases/latest/download/FullScreenWrapperForDLSS5.exe))
    - Or find it under the latest [Release](https://github.com/ThioJoe/Full-Screen-DLSS5-Wrapper/releases) under "Assets"
2. Acquire `nvngx_dlssnr.dll` and put it next to the `exe` (see [Requirements](https://github.com/ThioJoe/Full-Screen-DLSS5-Wrapper#requirements) section below)
3. Run the `exe` (no installation required).
4. By default, it applies to the primary monitor, with more options in the View tab. You can also apply it to a specific selected window.

## Key Features:

- **Full control** over internal model inputs. Including uncapped values for structure and tone.
- Ability to selectively apply it to a **specific window**.
- Built in screenshot and recording features:
  - **Simultaneously capture** both the original image/video and its processed output as separate files.
    - The video files are also perfectly synchronized frame-by-frame for easy comparison.
  - **Comparison Capture**: Automatically capture screenshots with multiple settings combinations for the same frame

## Why This Over Similar Tools?
- It's a single `.exe` file, no installation or third party dependencies required.
    - It's also signed with a trusted certificate.
- Designed to Minimize Anti-Cheat False-Positive Risk
  - Does NOT inject itself into or modify any other applications. It captures the final screen output using Windows' screen-capture APIs and processes that. (See [How It Works](https://github.com/ThioJoe/Full-Screen-DLSS5-Wrapper#how-it-works) explanation below)
    - This makes it architecturally more similar to screen-capture software or an external graphics-enhancement overlay.
  - It's code signed, so any anti-cheat providers could see you are running the unmodified version of the tool, which independently verifies loaded Nvidia binary signatures.
  - Note: There's still never a guarantee. Some anti-cheats may block overlays in general. So still best to not use it in competitive games, or where third party overlays are prohibited. Check the game's rules.

# Example Screenshots

> [!IMPORTANT]
> The output of this app will look different than if used with a natively supported DLSS5 game.
> 
> Native DLSS5 Games can provide the model with much more data such as motion data, object depth info, exact skin maps, etc to create a better result.
>
> This also means the performance/fps output from this app is NOT what the performance would be from a natively implemented game.
> 
> The output also obviously depends on the settings, which can be set to extreme values using this app, but would never actually be used in a real game.


<h3 align="center">Original Screenshot:</h3>
<p align="center">
<img width="1000" alt="Oblivion Original" src="https://github.com/user-attachments/assets/c75de803-3a08-4025-9d76-cfcaa7922996" />
</p>

<h3 align="center">Standard Tone and Structure Settings:</h3>
<p align="center">
<img width="1000" alt="Oblivion Standard" src="https://github.com/user-attachments/assets/8af48486-0b97-4d8a-a5f7-8b2b930f9006" />
</p>

<h3 align="center">Standard + 3X Multiplier:</h3>
<p align="center">
<img width="1000" alt="Oblivion 3x Standard" src="https://github.com/user-attachments/assets/7f6b574f-2143-4d25-bc00-032c2d24f38a" />
</p>

<h3 align="center">Standard + 20X Multiplier:</h3>
<p align="center">
<img width="1000" alt="Oblivion 20x" src="https://github.com/user-attachments/assets/9f2422c9-cc0c-4bff-81cc-02a0c210882b" />
</p>

------


# Requirements

- You need a 50-Series Nvidia GPU and Nvidia drivers `616.64` or newer
- You must acquire `nvngx_dlssnr.dll` yourself and put it next to the app `exe`.
  - Just Google it, you can find people who have uploaded it like on reddit.
  - For copyright reasons I will not host or link to it from here.
  - Note: The app will verify the dll's signature to ensure it's the right file either way.

### Optional:
- `nvngx_dlss.dll` - Enables use of super resolution options. Also put that next to the `exe`.

------

# How It Works
On a simple level, it:
1. Captures the regular screen output (or a screen region) using the Windows API
2. Passes the video stream into the DLSS5 model (which is in the `dll` file), along with chosen processing options.
3. Receives the new processed video data
4. Creates a brand new borderless window that is shown on top.
    - In other words, it doesn't directly modify the other apps themselves. They still are technically showing their original windows underneath it. Almost as if you put a video camera recording the screen, which applies effects then outputs to a second monitor. Except in this case it's a new window.
    - This window is "click through", so it is effectively invisible to the cursor. This means you can click, hover, and interact with everything beneath just as you normally would.
    - If you have it set to affect only a specific window, it only covers that window. If set to apply to the whole screen, the window covers the entire screen. 

For a much more detailed and technical explanation, see the [Advanced Readme](https://github.com/ThioJoe/Full-Screen-DLSS5-Wrapper/blob/main/Readme_Advanced.md) file  (`Readme_Advanced.md`).

## Optional: Launch via Commandline
- The tool can be launched by simply double clicking the `exe` to launch the GUI, or via the command line.
- If you launch the app from command line, it will output debug info there. There are also file logging options as CLI arguments.
- Use the `--help` argument to see all command line options (mostly the same options as in the GUI)

-----

# Screenshots of Other App Tabs

<h3 align="center">Capture Tab</h3>
<p align="center">
    <img width="605" alt="Full-Screen Wrapper for DLSS5 Capture Tab" src="https://github.com/user-attachments/assets/be3883b2-7cbb-41e0-96f0-473f2fb08671" />
</p>

<h3 align="center">Advanced Tab</h3>
<p align="center">
    <img width="605" alt="Full-Screen Wrapper for DLSS5 Advanced Tab" src="https://github.com/user-attachments/assets/996b1d71-3ebe-4356-996a-71decbb5bc82" />
</p>

<h3 align="center">View Tab</h3>
<p align="center">
    <img width="605" alt="Full-Screen Wrapper for DLSS5 View Tab" src="https://github.com/user-attachments/assets/cfdf76c1-8a81-4d3b-ad6b-360324cec3b4" />
</p>


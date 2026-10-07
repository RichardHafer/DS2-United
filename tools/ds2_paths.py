"""Dead Space 2 install paths shared by the offline tools.

Set DS2_GAME_DIR to point the tools at another install, and DS2_MODDING_TOOLS
if the modding tools do not live inside the game folder.
"""
import os
from pathlib import Path

GAME_DIR = Path(os.environ.get("DS2_GAME_DIR") or "D:/SteamLibrary/steamapps/common/Dead Space 2")
MODDING_TOOLS = Path(os.environ.get("DS2_MODDING_TOOLS") or GAME_DIR / "Dead Space 2 Modding Tools")
WORK = GAME_DIR / "work"

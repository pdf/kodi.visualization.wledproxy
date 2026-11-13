"""Opens the settings dialog of the visualization the proxy wraps.

Run by the "Configure visualization" button. The button closes and saves the
proxy's own dialog first, so a target chosen just before is the one opened.
"""

import xbmc
import xbmcaddon

PROXY_ID = "visualization.wledproxy"


def main():
    target = xbmcaddon.Addon(PROXY_ID).getSetting("target")
    if not target or target == PROXY_ID:
        xbmc.log(PROXY_ID + ": no visualization chosen", xbmc.LOGWARNING)
        return
    xbmc.executebuiltin("Addon.OpenSettings(%s)" % target)


if __name__ == "__main__":
    main()

import wapiIconUrl from "../wapi.png";

const bridge = window.wapi ?? {
  window: {
    minimize: async () => null,
    toggleMaximize: async () => null,
    close: async () => null
  }
};

function installRendererIcon() {
  const existing = document.querySelector("link[rel='icon']");
  const link = existing || document.createElement("link");
  link.rel = "icon";
  link.type = "image/png";
  link.href = wapiIconUrl;
  if (!existing) document.head.appendChild(link);
}

function installStyles() {
  const style = document.createElement("style");
  style.textContent = `
    :root {
      color-scheme: dark;
      font-family: -apple-system, BlinkMacSystemFont, "SF Pro Text", "Segoe UI", sans-serif;
      background: #050505;
    }

    * {
      box-sizing: border-box;
    }

    html,
    body,
    #root {
      width: 100%;
      height: 100%;
      margin: 0;
      overflow: hidden;
    }

    body {
      color: #d8d8d8;
      background: #050505;
      -webkit-user-select: none;
      user-select: none;
    }

    .app-shell {
      width: 100%;
      height: 100%;
      background: #050505;
    }

    #windowChrome {
      position: fixed;
      top: 0;
      left: 0;
      right: 0;
      z-index: 10;
      display: flex;
      align-items: center;
      justify-content: space-between;
      height: 40px;
      min-height: 40px;
      padding: 0 12px;
      border-bottom: 1px solid rgba(255, 255, 255, 0.08);
      background: #060606;
      -webkit-app-region: drag;
    }

    #windowTitleGroup {
      display: flex;
      align-items: center;
      gap: 8px;
      min-width: 0;
      height: 100%;
      pointer-events: none;
    }

    #windowTitleIcon {
      width: 22px;
      height: 22px;
      object-fit: contain;
      flex: 0 0 auto;
    }

    #versionLabel {
      overflow: hidden;
      color: #a5a5a5;
      font-size: 12px;
      line-height: 1;
      text-overflow: ellipsis;
      white-space: nowrap;
    }

    #windowButtons {
      display: flex;
      align-items: center;
      gap: 15px;
      height: 100%;
      -webkit-app-region: no-drag;
    }

    .window-btn {
      position: relative;
      display: flex;
      align-items: center;
      justify-content: center;
      flex: 0 0 24px;
      width: 24px;
      height: 24px;
      margin: 0;
      padding: 0;
      border: none;
      outline: none;
      color: #9b9b9b;
      background: transparent;
      cursor: pointer;
      line-height: 1;
    }

    .window-btn:hover {
      color: #ffffff;
      background: transparent;
    }

    .window-icon {
      position: absolute;
      left: 50%;
      top: 50%;
      display: block;
      width: 12px;
      height: 12px;
      transform: translate(-50%, -50%);
      pointer-events: none;
    }

    .window-icon-min::before {
      position: absolute;
      left: 1px;
      right: 1px;
      top: 50%;
      height: 1.6px;
      border-radius: 2px;
      background: currentColor;
      content: "";
      transform: translateY(-50%);
    }

    .window-icon-max::before {
      position: absolute;
      inset: 1px;
      border: 1.4px solid currentColor;
      border-radius: 1.5px;
      content: "";
    }

    .window-icon-close::before,
    .window-icon-close::after {
      position: absolute;
      left: 0;
      top: 50%;
      width: 12px;
      height: 1.4px;
      border-radius: 1px;
      background: currentColor;
      content: "";
      transform-origin: center;
    }

    .window-icon-close::before {
      transform: rotate(45deg);
    }

    .window-icon-close::after {
      transform: rotate(-45deg);
    }

    .blank-stage {
      width: 100%;
      height: 100%;
      padding-top: 40px;
      background: #050505;
    }
  `;
  document.head.appendChild(style);
}

function renderWindowBar() {
  const root = document.getElementById("root");
  root.innerHTML = `
    <main class="app-shell">
      <header id="windowChrome">
        <div id="windowTitleGroup">
          <img id="windowTitleIcon" src="${wapiIconUrl}" alt="">
          <span id="versionLabel">Wapi</span>
        </div>
        <div id="windowButtons">
          <button id="windowMinimize" class="window-btn window-btn-min" type="button" aria-label="Minimize">
            <span class="window-icon window-icon-min"></span>
          </button>
          <button id="windowMaximize" class="window-btn window-btn-max" type="button" aria-label="Maximize">
            <span class="window-icon window-icon-max"></span>
          </button>
          <button id="windowClose" class="window-btn window-btn-close" type="button" aria-label="Close">
            <span class="window-icon window-icon-close"></span>
          </button>
        </div>
      </header>
      <section class="blank-stage" aria-label="Empty workspace"></section>
    </main>
  `;

  document.getElementById("windowMinimize")?.addEventListener("click", () => bridge.window.minimize());
  document.getElementById("windowMaximize")?.addEventListener("click", () => bridge.window.toggleMaximize());
  document.getElementById("windowClose")?.addEventListener("click", () => bridge.window.close());
}

installRendererIcon();
installStyles();
renderWindowBar();

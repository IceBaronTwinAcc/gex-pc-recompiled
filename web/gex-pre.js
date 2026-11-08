var gexDataDependency = "gex-game-data";
var gexImportWaiting = true;
var gexFileSystemReady = false;
var gexQueuedFiles = null;
var gexMenuChoiceAddress = 0;
var gexMenuStack = [];

Module.preRun = Module.preRun || [];
Module.preRun.push(function () {
  addRunDependency(gexDataDependency);
  gexFileSystemReady = true;
  if (gexQueuedFiles) {
    var files = gexQueuedFiles;
    gexQueuedFiles = null;
    Module.gexImportGameFiles(files);
  }
});

Module.gexImportGameFiles = async function (fileList) {
  if (!gexFileSystemReady) {
    gexQueuedFiles = Array.from(fileList);
    window.gexSetStatus("Preparing the WebAssembly runtime; game files will load when it is ready.");
    return;
  }
  if (!gexImportWaiting) {
    window.gexSetStatus("Game data has already been loaded. Reload the page to select a different folder.", true);
    return;
  }
  var files = Array.from(fileList);
  if (!files.length) return;
  var root = null;
  var total = 0;
  var paths = [];
  for (var file of files) {
    var parts = file.webkitRelativePath.split("/");
    if (parts.length < 2 || parts.some(function (part) {
      return !part || part === "." || part === ".." || part.includes("\\");
    })) {
      window.gexSetStatus("The selected directory contains an invalid relative path.", true);
      return;
    }
    if (root === null) root = parts[0];
    if (parts[0] !== root) {
      window.gexSetStatus("Select one game folder at a time.", true);
      return;
    }
    var relative = parts.slice(1);
    if (relative.length === 1 && relative[0].toLowerCase() === "gex.exe")
      relative[0] = "GEX.exe";
    paths.push({ file: file, path: "/Gex/" + relative.join("/") });
    total += file.size;
  }
  if (!paths.some(function (entry) { return entry.path === "/Gex/GEX.exe"; })) {
    window.gexSetStatus("The selected folder does not contain GEX.exe at its root.", true);
    return;
  }
  if (total > 1073741824) {
    window.gexSetStatus("The selected folder is over the 1 GiB import limit for this experimental build.", true);
    return;
  }

  window.gexSetStatus("Loading " + files.length + " files (" +
    (total / 1048576).toFixed(0) + " MiB) into browser memory. This may take several minutes; keep this page open.");
  try {
    FS.mkdirTree("/Gex");
    var loaded = 0;
    for (var entry of paths) {
      var slash = entry.path.lastIndexOf("/");
      FS.mkdirTree(entry.path.slice(0, slash));
      var bytes = new Uint8Array(await entry.file.arrayBuffer());
      FS.writeFile(entry.path, bytes);
      loaded += entry.file.size;
      window.gexSetStatus("Loading game files: " + Math.floor(loaded * 100 / total) +
        "% (" + (loaded / 1048576).toFixed(0) + " / " + (total / 1048576).toFixed(0) + " MiB)");
    }
    gexImportWaiting = false;
    removeRunDependency(gexDataDependency);
    window.gexSetStatus("Game files loaded. Starting Gex...");
    document.getElementById("canvas").focus();
  } catch (error) {
    window.gexSetStatus("Could not load game data into browser memory: " +
      (error && error.message ? error.message : String(error)) +
      ". Free browser memory, reload, and try again.", true);
  }
};

function gexWriteMenuChoice(choice) {
  if (gexMenuChoiceAddress && Module.HEAP32)
    Atomics.store(Module.HEAP32, gexMenuChoiceAddress >> 2, choice);
  gexMenuChoiceAddress = 0;
  var backdrop = document.getElementById("menu-backdrop");
  backdrop.setAttribute("aria-hidden", "true");
  gexMenuStack = [];
}

Module.gexChooseMenu = gexWriteMenuChoice;
Module.gexPopupMenu = function (json, choiceAddress) {
  if (gexMenuChoiceAddress) gexWriteMenuChoice(-1);
  gexMenuChoiceAddress = choiceAddress;
  try {
    gexMenuStack = [JSON.parse(json)];
    gexRenderMenu();
    document.getElementById("menu-backdrop").setAttribute("aria-hidden", "false");
    var first = document.querySelector("#menu-items button");
    if (first) first.focus();
  } catch (error) {
    gexWriteMenuChoice(-1);
    window.gexSetStatus("Could not display the game menu: " + error, true);
  }
};

function gexRenderMenu() {
  var container = document.getElementById("menu-items");
  var back = document.getElementById("menu-back");
  back.hidden = gexMenuStack.length <= 1;
  container.replaceChildren();
  gexMenuStack[gexMenuStack.length - 1].forEach(function (item) {
    if (!item || !item.label) {
      container.appendChild(document.createElement("hr"));
      return;
    }
    var button = document.createElement("button");
    button.type = "button";
    var label = document.createElement("span");
    label.textContent = item.label;
    button.appendChild(label);
    if (item.checked) {
      var checked = document.createElement("span");
      checked.textContent = "✓";
      checked.setAttribute("aria-label", "selected");
      button.appendChild(checked);
    }
    if (item.hint) {
      var hint = document.createElement("small");
      hint.textContent = item.hint;
      button.appendChild(hint);
    }
    button.disabled = item.id === undefined && !item.sub;
    button.addEventListener("click", function () {
      if (item.sub && item.sub.length) {
        gexMenuStack.push(item.sub);
        gexRenderMenu();
      } else {
        gexWriteMenuChoice(item.id);
      }
    });
    container.appendChild(button);
  });
}

if (typeof document !== "undefined") {
  document.getElementById("menu-back").addEventListener("click", function () {
    if (gexMenuStack.length > 1) {
      gexMenuStack.pop();
      gexRenderMenu();
    }
  });
  document.addEventListener("keydown", function (event) {
    if (event.key === "Escape" &&
        document.getElementById("menu-backdrop").getAttribute("aria-hidden") === "false") {
      event.preventDefault();
      Module.gexChooseMenu(-1);
    }
  });
  document.getElementById("menu-backdrop").addEventListener("click", function (event) {
    if (event.target === event.currentTarget) Module.gexChooseMenu(-1);
  });
}

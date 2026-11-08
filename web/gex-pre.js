var gexDataDependency = "gex-game-data";
var gexImportWaiting = true;
var gexFileSystemReady = false;
var gexStorageChecking = true;
var gexImportInProgress = false;
var gexHasSavedGame = false;
var gexQueuedFiles = null;
var gexMenuChoiceAddress = 0;
var gexMenuStack = [];

function gexPickerEnabled(enabled) {
  document.getElementById("choose-folder").disabled = !enabled;
  document.getElementById("remember-game").disabled = !enabled;
}

function gexOpenGameDatabase() {
  return new Promise(function (resolve, reject) {
    var request;
    try {
      request = indexedDB.open("gex-recomp-game:" + new URL(".", window.location.href).pathname, 1);
    } catch (error) {
      reject(error);
      return;
    }
    var blocked = false;
    request.onupgradeneeded = function () {
      request.result.createObjectStore("files", {keyPath: "path"});
    };
    request.onblocked = function () {
      blocked = true;
      reject(new Error("browser storage is blocked by another tab"));
    };
    request.onerror = function () { reject(request.error); };
    request.onsuccess = function () {
      var db = request.result;
      db.onversionchange = function () { db.close(); };
      if (blocked) db.close();
      else resolve(db);
    };
  });
}

async function gexGameStorage(mode, entries) {
  var db = await gexOpenGameDatabase();
  try {
    return await new Promise(function (resolve, reject) {
      var tx = db.transaction("files", mode === "read" ? "readonly" : "readwrite");
      var store = tx.objectStore("files");
      var request;
      try {
        if (mode === "read") request = store.getAll();
        else {
          store.clear();
          if (mode === "write") {
            for (var entry of entries) store.put({path: entry.path, file: entry.file});
          }
        }
      } catch (error) {
        tx.abort();
        reject(error);
        return;
      }
      tx.oncomplete = function () { resolve(request ? request.result : undefined); };
      tx.onabort = function () { reject(tx.error || new Error("browser storage transaction failed")); };
    });
  } finally {
    db.close();
  }
}

function gexValidateGameEntries(entries) {
  var seen = new Set();
  var total = 0;
  for (var entry of entries) {
    if (typeof entry.path !== "string" || !entry.path.startsWith("/Gex/") ||
        entry.path.slice(5).split("/").some(function (part) {
          return !part || part === "." || part === ".." || part.includes("\\");
        }) || !entry.file || typeof entry.file.arrayBuffer !== "function" ||
        !Number.isSafeInteger(entry.file.size) || entry.file.size < 0 ||
        seen.has(entry.path)) {
      throw new Error("game files contain invalid or duplicate paths");
    }
    seen.add(entry.path);
    total += entry.file.size;
  }
  if (!seen.has("/Gex/GEX.exe")) throw new Error("the selected folder does not contain GEX.exe at its root");
  if (!total) throw new Error("the game folder is empty");
  if (total > 1073741824) throw new Error("the game folder exceeds the 1 GiB import limit");
  return total;
}

async function gexLoadGameEntries(entries, total, saved) {
  gexImportInProgress = true;
  gexPickerEnabled(false);
  document.getElementById("forget-game").disabled = true;
  var loadingStarted = false;
  try {
    if (!saved && document.getElementById("remember-game").checked) {
      window.gexSetStatus("Saving game files in browser storage on this device.");
      try {
        await gexGameStorage("write", entries);
      } catch (error) {
        throw new Error("could not save game files in browser storage: " + error.message +
                        ". Free browser storage or uncheck 'Remember game files' and select the folder again");
      }
      saved = true;
      gexHasSavedGame = true;
    }
    document.getElementById("forget-game").disabled = !gexHasSavedGame;
    window.gexSetStatus("Loading " + entries.length + " files (" +
      (total / 1048576).toFixed(0) + " MiB) into browser memory.");
    loadingStarted = true;
    FS.mkdirTree("/Gex");
    var loaded = 0;
    for (var entry of entries) {
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
    gexPickerEnabled(!loadingStarted);
    document.getElementById("forget-game").disabled = !gexHasSavedGame;
    window.gexSetStatus("Could not load game data: " + error.message +
                        (loadingStarted ? ". Reload the page to retry." : "."), true);
  } finally {
    gexImportInProgress = false;
  }
}

async function gexCheckStoredGame() {
  window.gexSetStatus("Checking for saved game files on this device.");
  var entries;
  try {
    entries = await gexGameStorage("read");
  } catch (error) {
    gexStorageChecking = false;
    gexPickerEnabled(true);
    document.getElementById("remember-game").checked = false;
    window.gexSetStatus("Browser storage is unavailable: " + error.message +
                        ". Select a game folder to play this session without saving it.", true);
    if (gexQueuedFiles) {
      var queued = gexQueuedFiles;
      gexQueuedFiles = null;
      Module.gexImportGameFiles(queued);
    }
    return;
  }
  gexStorageChecking = false;
  gexHasSavedGame = entries.length > 0;
  document.getElementById("forget-game").disabled = !gexHasSavedGame;
  if (gexQueuedFiles) {
    var queued = gexQueuedFiles;
    gexQueuedFiles = null;
    Module.gexImportGameFiles(queued);
    return;
  }
  if (!entries.length) {
    gexPickerEnabled(true);
    window.gexSetStatus("Choose your game folder to start.");
    return;
  }
  try {
    await gexLoadGameEntries(entries, gexValidateGameEntries(entries), true);
  } catch (error) {
    gexPickerEnabled(true);
    window.gexSetStatus("Saved game files are invalid: " + error.message +
                        ". Remove them or select the game folder again.", true);
  }
}

Module.preRun = Module.preRun || [];
Module.preRun.push(function () {
  addRunDependency(gexDataDependency);
  gexFileSystemReady = true;
  gexCheckStoredGame();
});

Module.gexImportGameFiles = async function (fileList) {
  if (!gexFileSystemReady || gexStorageChecking) {
    gexQueuedFiles = Array.from(fileList);
    window.gexSetStatus("Preparing the WebAssembly runtime; game files will load when it is ready.");
    return;
  }
  if (!gexImportWaiting || gexImportInProgress) {
    window.gexSetStatus("Game data has already been loaded. Reload the page to select a different folder.", true);
    return;
  }
  var files = Array.from(fileList);
  if (!files.length) return;
  var root = null;
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
  }
  try {
    await gexLoadGameEntries(paths, gexValidateGameEntries(paths), false);
  } catch (error) {
    window.gexSetStatus("Could not load game data: " + error.message + ".", true);
  }
};

Module.gexForgetSavedGame = async function () {
  if (gexStorageChecking || gexImportInProgress) return;
  document.getElementById("forget-game").disabled = true;
  try {
    await gexGameStorage("clear");
    gexHasSavedGame = false;
    window.gexSetStatus(gexImportWaiting
      ? "Saved game files removed. Choose a game folder to start."
      : "Saved game files removed. Reload to select a game folder.");
  } catch (error) {
    document.getElementById("forget-game").disabled = false;
    window.gexSetStatus("Could not remove saved game files: " + error.message, true);
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

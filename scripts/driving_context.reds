@if(ModuleExists("HudPainter"))
import HudPainter.{ComponentColorItem, CustomColorPickerController, HudPainterStorage}
@if(ModuleExists("HudPainter"))
import Codeware.UI.TextInput
@if(ModuleExists("HudPainter"))
import RedData.Json.{JsonArray, JsonObject, JsonVariant, ParseJson}
@if(ModuleExists("HudPainter"))
import RedFileSystem.{File, FileSystem, FileSystemStatus, FileSystemStorage}

private func ChromeAxis_IsFrenchInterface(game: GameInstance) -> Bool {
  let settings: ref<UserSettings> = GameInstance.GetSettingsSystem(game);
  if !IsDefined(settings) {
    return false;
  }
  let language: ref<ConfigVarListName> = settings.GetVar(n"/language", n"OnScreen") as ConfigVarListName;
  return IsDefined(language) && Equals(language.GetValue(), n"fr-fr");
}

class ChromeAxisHudPalette {
  public let border: HDRColor;
  public let transmission: HDRColor;

  public static func Default() -> ref<ChromeAxisHudPalette> {
    let palette = new ChromeAxisHudPalette();
    palette.border = HDRColor(73.0 / 255.0, 222.0 / 255.0, 238.0 / 255.0, 1.0);
    palette.transmission = HDRColor(1.0, 82.0 / 255.0, 94.0 / 255.0, 1.0);
    return palette;
  }
}

@if(!ModuleExists("HudPainter"))
private func ChromeAxis_LoadHudPalette() -> ref<ChromeAxisHudPalette> {
  return ChromeAxisHudPalette.Default();
}

@if(ModuleExists("HudPainter"))
private func ChromeAxis_LoadHudPalette() -> ref<ChromeAxisHudPalette> {
  let menu = ChromeAxisHudPainterMenu.Get();
  if IsDefined(menu) {
    let palette = menu.GetPalette();
    if IsDefined(palette) {
      return palette;
    }
  }
  return ChromeAxisHudPalette.Default();
}

@if(ModuleExists("HudPainter"))
class ChromeAxisHudPaletteEdit {
  public let key: String;
  public let border: Bool;
  public let transmission: Bool;
  public let palette: ref<ChromeAxisHudPalette>;
}

@if(ModuleExists("HudPainter"))
class ChromeAxisHudPresetWrite {
  public let key: String;
  public let palette: ref<ChromeAxisHudPalette>;
  public let session: Int32;
}

@if(ModuleExists("HudPainter"))
class ChromeAxisHudPresetWriteBoundary extends Event {
  public let write: ref<ChromeAxisHudPresetWrite>;
  public let begin: Bool;
  public let observed: Bool;
}

@if(ModuleExists("HudPainter"))
enum ChromeAxisHudSaveStatus {
  Available = 0,
  FileUnavailable = 1,
  InvalidPreset = 2,
  OtherPresetPending = 3,
  RetryingFile = 4,
  InvalidFile = 5,
  EmptyFile = 6,
  FileTooLarge = 7,
  ReloadingExternalEdit = 8,
  ExternalEditPending = 9,
  RecoveryRequired = 10,
  RecoveryScanDenied = 11
}

@if(ModuleExists("HudPainter"))
enum ChromeAxisHudFileState {
  // Values 0–6 and 9 match the results of the native storage functions.
  Available = 0,
  Busy = 1,
  Conflict = 2,
  Empty = 3,
  TooLarge = 4,
  Unavailable = 5,
  RecoveryRequired = 6,
  Invalid = 7,
  ReadFailed = 8,
  RecoveryScanDenied = 9
}

@if(ModuleExists("HudPainter"))
private func ChromeAxis_HudFileState(result: Int32) -> ChromeAxisHudFileState {
  return (result >= 0 && result <= 6) || result == 9 ? IntEnum<ChromeAxisHudFileState>(result) : ChromeAxisHudFileState.Unavailable;
}

@if(ModuleExists("HudPainter"))
private func ChromeAxis_HudReadFailure(probeReadable: Bool) -> ChromeAxisHudFileState {
  if !probeReadable {
    return ChromeAxisHudFileState.ReadFailed;
  }
  // RedFileSystem returns no reason for a failed text read. The native probe
  // distinguishes a Windows share lock from a permission or persistent error.
  let result = ChromeAxis_HudFileState(ChromeAxis_RecoverHudColors(true));
  return Equals(result, ChromeAxisHudFileState.Available) ? ChromeAxisHudFileState.ReadFailed : result;
}

@if(ModuleExists("HudPainter"))
private func ChromeAxis_HudColorsMatch(first: HDRColor, second: HDRColor) -> Bool {
  return Equals(first.Red, second.Red) && Equals(first.Green, second.Green) && Equals(first.Blue, second.Blue);
}

@if(ModuleExists("HudPainter"))
private func ChromeAxis_HudSliderValue(channel: Float) -> Int32 {
  return Cast<Int32>(ClampF(channel * 255.0, 0.0, 400.0));
}

@if(ModuleExists("HudPainter"))
class ChromeAxisHudPainterMenu extends ScriptableService {
  private let storage: ref<FileSystemStorage>;
  private let colors: ref<JsonObject>;
  private let fileStatus: ChromeAxisHudSaveStatus;
  private let palette: ref<ChromeAxisHudPalette>;
  private let activePresetKey: String;
  private let edits: array<ref<ChromeAxisHudPaletteEdit>>;
  private let flushCallback: ref<ChromeAxisHudColorFlush>;
  private let flushDelaySystem: wref<DelaySystem>;
  private let flushDelayId: DelayID;
  private let retryDelay: Float;
  private let retryAttempts: Int32;
  private let reloadPending: Bool;
  private let presetWriteSession: Int32;
  private let activePresetWrite: ref<ChromeAxisHudPresetWrite>;
  private let observedPresetSaves: array<wref<HudPainterPresetSaved>>;
  private let menuController: wref<inkGameController>;
  private let section: wref<ChromeAxisHudPainterSection>;
  private let picker: wref<CustomColorPickerController>;

  public static func Get() -> ref<ChromeAxisHudPainterMenu> {
    return GameInstance.GetScriptableServiceContainer().GetService(n"ChromeAxisHudPainterMenu") as ChromeAxisHudPainterMenu;
  }

  private cb func OnLoad() {
    // RedFileSystem grants this storage handle once per game launch.
    this.storage = FileSystem.GetStorage("ChromeAxisHUD");
    this.palette = ChromeAxisHudPalette.Default();
    this.RefreshColors();
    GameInstance.GetCallbackSystem()
      .RegisterCallback(n"InkWidget/Spawn", this, n"OnHudPainterWidgetSpawn")
      .AddTarget(inkWidgetTarget.Library(r"base\\gameplay\\gui\\hud_painter.inkwidget", n"ColorPicker"))
      .AddTarget(inkWidgetTarget.Controller(n"HudPainter.CustomColorPickerController"));
  }

  private cb func OnInitialize() {
    GameInstance.GetCallbackSystem().RegisterCallback(n"Session/Ready", this, n"OnSessionReady");
    GameInstance.GetCallbackSystem().RegisterCallback(n"Session/Resume", this, n"OnSessionResume");
    if this.reloadPending && !IsDefined(this.flushCallback) {
      this.FlushColors(true);
    }
  }

  private cb func OnUninitialize() {
    GameInstance.GetCallbackSystem().UnregisterCallback(n"Session/Ready", this);
    GameInstance.GetCallbackSystem().UnregisterCallback(n"Session/Resume", this);
    this.presetWriteSession += 1;
    this.activePresetWrite = null;
    ArrayClear(this.observedPresetSaves);
    this.CancelScheduledFlush();
  }

  private cb func OnSessionReady(event: ref<GameSessionEvent>) {
    // A new game session invalidates outstanding UI write markers.
    this.presetWriteSession += 1;
    this.activePresetWrite = null;
    this.RefreshColors(true);
  }

  private cb func OnSessionResume(event: ref<GameSessionEvent>) {
    // A paused session may delay the flush without invalidating its pending save.
    this.RefreshColors(true);
  }

  public func GetPalette() -> ref<ChromeAxisHudPalette> {
    if Equals(this.activePresetKey, "") {
      this.SyncActivePreset();
    }
    return this.palette;
  }

  public func SyncActivePreset() -> Void {
    let painter = HudPainterStorage.Get();
    if !IsDefined(painter) {
      return;
    }
    let name = painter.GetActivePresetName();
    let key = StrLower(name);
    if Equals(key, "") || Equals(key, this.activePresetKey) {
      return;
    }

    this.activePresetKey = key;
    this.RefreshColors();
  }

  public func ChangeColor(isBorder: Bool, color: HDRColor) -> Void {
    let current = this.GetPalette();
    let previous = isBorder ? current.border : current.transmission;
    // Opening the picker can emit updates for its own integer projection of a saved color.
    if Equals(this.activePresetKey, "") ||
      (Equals(ChromeAxis_HudSliderValue(previous.Red), ChromeAxis_HudSliderValue(color.Red)) &&
       Equals(ChromeAxis_HudSliderValue(previous.Green), ChromeAxis_HudSliderValue(color.Green)) &&
       Equals(ChromeAxis_HudSliderValue(previous.Blue), ChromeAxis_HudSliderValue(color.Blue))) {
      return;
    }
    let next = new ChromeAxisHudPalette();
    next.border = isBorder ? color : current.border;
    next.transmission = isBorder ? current.transmission : color;
    this.palette = next;
    this.StorePalette(this.activePresetKey, next, isBorder, !isBorder);
    this.ApplyToHud();

    // Batch slider changes. Release and menu close also flush while the game is paused.
    this.ScheduleFlush(0.30);
  }

  private func ScheduleFlush(seconds: Float) -> Bool {
    if IsDefined(this.flushCallback) {
      return false;
    }
    let delay = GameInstance.GetDelaySystem(GetGameInstance());
    if !IsDefined(delay) {
      return false;
    }
    let callback = new ChromeAxisHudColorFlush();
    callback.menu = this;
    this.flushDelaySystem = delay;
    this.flushDelayId = delay.DelayCallback(callback, seconds, false);
    this.flushCallback = callback;
    return true;
  }

  private func CancelScheduledFlush() -> Void {
    if IsDefined(this.flushCallback) {
      this.flushCallback = null;
      if IsDefined(this.flushDelaySystem) {
        this.flushDelaySystem.CancelDelay(this.flushDelayId);
      }
      this.flushDelaySystem = null;
    }
  }

  private func EndFlush() -> Void {
    this.CancelScheduledFlush();
    this.ResetRetryBudget();
  }

  private func ResetRetryBudget() -> Void {
    this.retryDelay = 0.0;
    this.retryAttempts = 0;
  }

  public func FlushAfterInput(callback: wref<ChromeAxisHudColorFlush>) -> Void {
    let scheduledCallback: wref<ChromeAxisHudColorFlush> = this.flushCallback;
    if NotEquals(scheduledCallback, callback) {
      return;
    }
    this.flushCallback = null;
    this.flushDelaySystem = null;
    this.FlushColors(true);
  }

  public func BeginPresetSave(name: String) -> ref<ChromeAxisHudPresetWrite> {
    let write = new ChromeAxisHudPresetWrite();
    write.key = StrLower(name);
    write.palette = this.GetPalette();
    write.session = this.presetWriteSession;
    this.QueuePresetWriteBoundary(write, true);
    return write;
  }

  public func FinishPresetSave(write: ref<ChromeAxisHudPresetWrite>) -> Void {
    this.QueuePresetWriteBoundary(write, false);
  }

  private func QueuePresetWriteBoundary(write: ref<ChromeAxisHudPresetWrite>, begin: Bool) -> Void {
    let uiSystem = GameInstance.GetUISystem(GetGameInstance());
    if IsDefined(uiSystem) {
      // HUD Painter queues its unnamed success event during the synchronous write.
      let event = new ChromeAxisHudPresetWriteBoundary();
      event.write = write;
      event.begin = begin;
      uiSystem.QueueEvent(event);
    }
  }

  public func ObservePresetWriteBoundary(event: ref<ChromeAxisHudPresetWriteBoundary>) -> Void {
    if event.observed || event.write.session != this.presetWriteSession {
      return;
    }
    event.observed = true;
    if event.begin {
      this.activePresetWrite = event.write;
    } else if Equals(this.activePresetWrite, event.write) {
      this.activePresetWrite = null;
    }
  }

  public func PresetSaved(event: ref<HudPainterPresetSaved>) -> Void {
    // The same event can reach the menu component and several game controllers.
    let received: wref<HudPainterPresetSaved> = event;
    let index = ArraySize(this.observedPresetSaves) - 1;
    while index >= 0 {
      let previous = this.observedPresetSaves[index];
      if Equals(previous, received) {
        return;
      }
      if !IsDefined(previous) {
        ArrayErase(this.observedPresetSaves, index);
      }
      index -= 1;
    }
    ArrayPush(this.observedPresetSaves, received);

    let write = this.activePresetWrite;
    if IsDefined(write) {
      this.activePresetWrite = null;
      this.StorePalette(write.key, write.palette, true, true);
      this.FlushColors();
    }
  }

  public func ApplyToHud() -> Void {
    let systems = GameInstance.GetScriptableSystemsContainer(GetGameInstance());
    if IsDefined(systems) {
      let context = systems.Get(n"ChromeAxisDrivingContext") as ChromeAxisDrivingContext;
      if IsDefined(context) {
        context.ApplyHudPalette();
      }
    }
  }

  private func FindEdit(key: String) -> ref<ChromeAxisHudPaletteEdit> {
    for edit in this.edits {
      if Equals(edit.key, key) {
        return edit;
      }
    }
    return null;
  }

  private func StorePalette(key: String, palette: ref<ChromeAxisHudPalette>, border: Bool, transmission: Bool) -> Void {
    if Equals(key, "") {
      return;
    }
    if !IsDefined(this.flushCallback) {
      this.ResetRetryBudget();
    }
    let edit = this.FindEdit(key);
    if !IsDefined(edit) {
      edit = new ChromeAxisHudPaletteEdit();
      edit.key = key;
      edit.palette = new ChromeAxisHudPalette();
      ArrayPush(this.edits, edit);
    }
    if border {
      edit.border = true;
      edit.palette.border = palette.border;
    }
    if transmission {
      edit.transmission = true;
      edit.palette.transmission = palette.transmission;
    }
  }

  private func SavedPaletteForPreset(key: String) -> ref<ChromeAxisHudPalette> {
    // Pre-0.4.50 colors were global. Use them as defaults for each preset.
    let preset: ref<JsonObject>;
    if IsDefined(this.colors) {
      let presets = this.colors.GetKey("presets") as JsonObject;
      if IsDefined(presets) {
        preset = presets.GetKey(key) as JsonObject;
      }
    }
    return this.ReadPalette(preset, this.ReadPalette(this.colors, ChromeAxisHudPalette.Default()));
  }

  private func UpdatePalette() -> Void {
    let saved = this.SavedPaletteForPreset(this.activePresetKey);
    let edit = this.FindEdit(this.activePresetKey);
    this.palette = new ChromeAxisHudPalette();
    this.palette.border = IsDefined(edit) && edit.border ? edit.palette.border : saved.border;
    this.palette.transmission = IsDefined(edit) && edit.transmission ? edit.palette.transmission : saved.transmission;
    if NotEquals(this.activePresetKey, "") {
      this.ApplyToHud();
    }
    if IsDefined(this.section) {
      this.section.SyncColors(saved, this.palette, this.GetSaveStatus(), this.picker);
    }
  }

  public func RefreshColors(opt fromTimer: Bool) -> Void {
    this.EndFlush();
    this.reloadPending = true;
    this.FlushColors(fromTimer);
  }

  public func FlushColors(opt fromTimer: Bool) -> Void {
    if !this.reloadPending && ArraySize(this.edits) == 0 {
      return;
    }
    // Read again so a temporarily inaccessible file and other presets remain intact.
    let state: ChromeAxisHudFileState;
    let previous: String;
    let existed: Bool;
    let colors = this.ReadColors(state, previous, existed);
    if Equals(state, ChromeAxisHudFileState.RecoveryRequired) || Equals(state, ChromeAxisHudFileState.RecoveryScanDenied) {
      // Use the valid document for display. Leave edits pending until the recovery state is known.
      if IsDefined(colors) {
        this.colors = colors;
      }
      this.HandleFlushFailure(state, fromTimer && !IsDefined(colors));
      return;
    }
    if !IsDefined(colors) {
      this.HandleFlushFailure(state, fromTimer);
      return;
    }
    let presets = colors.GetKey("presets") as JsonObject;
    if !IsDefined(presets) {
      presets = new JsonObject();
    }
    let blocked: array<ref<ChromeAxisHudPaletteEdit>>;
    for edit in this.edits {
      if !this.MergePaletteEdit(presets, edit) {
        ArrayPush(blocked, edit);
      }
    }
    if ArraySize(blocked) < ArraySize(this.edits) {
      colors.SetKey("presets", presets);
      let result = ChromeAxis_HudFileState(ChromeAxis_CommitHudColors(colors.ToString(), previous, existed));
      if NotEquals(result, ChromeAxisHudFileState.Available) {
        this.HandleFlushFailure(result, fromTimer);
        return;
      }
    }
    this.colors = colors;
    this.reloadPending = false;
    this.edits = blocked;
    this.fileStatus = ChromeAxisHudSaveStatus.Available;
    // Any leftovers belong to malformed presets. Leave them pending without rewriting the file.
    this.EndFlush();
    this.UpdatePalette();
  }

  private func HandleFlushFailure(state: ChromeAxisHudFileState, fromTimer: Bool) -> Void {
    this.reloadPending = true;
    let status = ChromeAxisHudSaveStatus.FileUnavailable;
    switch state {
      case ChromeAxisHudFileState.Busy:
        if this.ScheduleRetry() {
          status = ChromeAxisHudSaveStatus.RetryingFile;
        }
        break;
      case ChromeAxisHudFileState.Conflict:
        // The delayed flush reads again and reapplies only the edited channels.
        status = this.ScheduleRetry(3) ? ChromeAxisHudSaveStatus.ReloadingExternalEdit : ChromeAxisHudSaveStatus.ExternalEditPending;
        break;
      case ChromeAxisHudFileState.Empty:
        status = ChromeAxisHudSaveStatus.EmptyFile;
        this.ScheduleRetry(3);
        break;
      case ChromeAxisHudFileState.Invalid:
        status = ChromeAxisHudSaveStatus.InvalidFile;
        this.ScheduleRetry(3);
        break;
      case ChromeAxisHudFileState.ReadFailed:
        if this.ScheduleRetry(3) {
          status = ChromeAxisHudSaveStatus.RetryingFile;
        }
        break;
      case ChromeAxisHudFileState.TooLarge:
        status = ChromeAxisHudSaveStatus.FileTooLarge;
        this.CancelScheduledFlush();
        break;
      case ChromeAxisHudFileState.RecoveryRequired:
        status = ChromeAxisHudSaveStatus.RecoveryRequired;
        this.CancelScheduledFlush();
        break;
      case ChromeAxisHudFileState.RecoveryScanDenied:
        status = ChromeAxisHudSaveStatus.RecoveryScanDenied;
        this.ScheduleRetry(3);
        break;
      default:
        this.CancelScheduledFlush();
        break;
    }
    if !fromTimer || NotEquals(status, this.fileStatus) {
      this.fileStatus = status;
      this.UpdatePalette();
    }
  }

  private func ScheduleRetry(opt limit: Int32) -> Bool {
    if limit > 0 && this.retryAttempts >= limit {
      this.CancelScheduledFlush();
      return false;
    }
    let seconds = ClampF(this.retryDelay * 2.0, 1.0, 8.0);
    if this.ScheduleFlush(seconds) {
      this.retryDelay = seconds;
      if limit > 0 {
        this.retryAttempts += 1;
      }
    }
    return IsDefined(this.flushCallback);
  }

  private func MergePaletteEdit(presets: ref<JsonObject>, edit: ref<ChromeAxisHudPaletteEdit>) -> Bool {
    let entry = presets.GetKey(edit.key) as JsonObject;
    if !IsDefined(entry) {
      if presets.HasKey(edit.key) {
        return false;
      }
      entry = new JsonObject();
    }
    if edit.border {
      entry.SetKey("border", this.EncodeColor(edit.palette.border));
    }
    if edit.transmission {
      entry.SetKey("transmission", this.EncodeColor(edit.palette.transmission));
    }
    presets.SetKey(edit.key, entry);
    return true;
  }

  private func GetSaveStatus() -> ChromeAxisHudSaveStatus {
    if NotEquals(this.fileStatus, ChromeAxisHudSaveStatus.Available) {
      return this.fileStatus;
    }
    let presets = this.colors.GetKey("presets") as JsonObject;
    if IsDefined(presets) && presets.HasKey(this.activePresetKey)
      && !IsDefined(presets.GetKey(this.activePresetKey) as JsonObject) {
      return ChromeAxisHudSaveStatus.InvalidPreset;
    }
    return ArraySize(this.edits) > 0 ? ChromeAxisHudSaveStatus.OtherPresetPending : ChromeAxisHudSaveStatus.Available;
  }

  public func AttachSection(section: wref<ChromeAxisHudPainterSection>) -> Void {
    this.section = section;
    this.RefreshColors();
  }

  public func DetachSection(section: wref<ChromeAxisHudPainterSection>) -> Void {
    if Equals(this.section, section) {
      this.section = null;
      this.FlushColors();
    }
  }

  public func PreparePicker(color: ref<HudPainterColorItem>) -> Void {
    if IsDefined(this.picker) {
      ChromeAxis_SetPickerChannels(this.picker, color.customColor);
    }
  }

  private func ReadColors(out state: ChromeAxisHudFileState, out previous: String, out existed: Bool) -> ref<JsonObject> {
    state = ChromeAxisHudFileState.Unavailable;
    previous = "";
    existed = false;
    if !IsDefined(this.storage) {
      return null;
    }
    let present = this.storage.Exists("colors.json");
    if Equals(present, FileSystemStatus.Denied) {
      return null;
    }
    // Check recovery paths without reading the existing document twice.
    let recovery = ChromeAxis_HudFileState(ChromeAxis_RecoverHudColors(false));
    let scanDenied = Equals(recovery, ChromeAxisHudFileState.RecoveryScanDenied);
    if (Equals(recovery, ChromeAxisHudFileState.RecoveryRequired) || scanDenied) && Equals(present, FileSystemStatus.True) {
      let readState: ChromeAxisHudFileState;
      let colors = this.ReadPresentColors(readState, previous, scanDenied);
      existed = true;
      state = scanDenied && !IsDefined(colors) ? readState : recovery;
      return colors;
    }
    if NotEquals(recovery, ChromeAxisHudFileState.Available) && NotEquals(recovery, ChromeAxisHudFileState.Conflict) {
      state = recovery;
      return null;
    }
    if Equals(present, FileSystemStatus.False) || Equals(recovery, ChromeAxisHudFileState.Conflict) {
      present = this.storage.Exists("colors.json");
      if Equals(present, FileSystemStatus.False) {
        state = recovery;
        return Equals(recovery, ChromeAxisHudFileState.Available) ? new JsonObject() : null;
      }
    }
    if Equals(present, FileSystemStatus.Denied) {
      return null;
    }
    if !Equals(present, FileSystemStatus.True) {
      state = ChromeAxis_HudReadFailure(true);
      return null;
    }
    existed = true;
    return this.ReadPresentColors(state, previous, true);
  }

  private func ReadPresentColors(out state: ChromeAxisHudFileState, out previous: String, probeReadable: Bool) -> ref<JsonObject> {
    let isFile = this.storage.IsFile("colors.json");
    if !Equals(isFile, FileSystemStatus.True) {
      if Equals(isFile, FileSystemStatus.Denied) {
        state = ChromeAxisHudFileState.Unavailable;
      } else if Equals(isFile, FileSystemStatus.False) && Equals(this.storage.Exists("colors.json"), FileSystemStatus.True) {
        state = ChromeAxisHudFileState.Invalid;
      } else {
        state = ChromeAxis_HudReadFailure(probeReadable);
      }
      return null;
    }
    let file = this.storage.GetFile("colors.json");
    if !IsDefined(file) {
      state = ChromeAxis_HudReadFailure(probeReadable);
      return null;
    }
    let size = file.GetSize();
    // std::filesystem::file_size uses UINT64_MAX when the size cannot be read.
    if Equals(size, 18446744073709551615ul) || Equals(size, 0ul) {
      state = ChromeAxis_HudReadFailure(probeReadable);
      return null;
    }
    if size > 4194304ul {
      state = ChromeAxisHudFileState.TooLarge;
      return null;
    }
    previous = file.ReadAsText();
    if Equals(previous, "") {
      state = ChromeAxis_HudReadFailure(probeReadable);
      return null;
    }
    let colors = ParseJson(previous) as JsonObject;
    if !IsDefined(colors) || (colors.HasKey("presets") && !IsDefined(colors.GetKey("presets") as JsonObject)) {
      state = ChromeAxisHudFileState.Invalid;
      return null;
    }
    state = ChromeAxisHudFileState.Available;
    return colors;
  }

  private func ReadPalette(colors: ref<JsonObject>, fallback: ref<ChromeAxisHudPalette>) -> ref<ChromeAxisHudPalette> {
    if !IsDefined(colors) {
      return fallback;
    }
    let palette = new ChromeAxisHudPalette();
    palette.border = this.ReadColor(colors, "border", fallback.border);
    palette.transmission = this.ReadColor(colors, "transmission", fallback.transmission);
    return palette;
  }

  private func ReadColor(colors: ref<JsonObject>, key: String, fallback: HDRColor) -> HDRColor {
    let channels = colors.GetKey(key) as JsonArray;
    if !IsDefined(channels) || NotEquals(channels.GetSize(), 3u) {
      return fallback;
    }
    let red = this.ReadChannel(channels.GetItem(0u));
    let green = this.ReadChannel(channels.GetItem(1u));
    let blue = this.ReadChannel(channels.GetItem(2u));
    if red < 0.0 || green < 0.0 || blue < 0.0 {
      return fallback;
    }
    return HDRColor(red, green, blue, 1.0);
  }

  private func ReadChannel(value: ref<JsonVariant>) -> Float {
    if !IsDefined(value) || !(value.IsInt64() || value.IsUint64() || value.IsDouble()) {
      return -1.0;
    }
    let channel = Cast<Float>(value.GetDouble());
    if channel >= 0.0 && channel <= 400.0 {
      return channel / 255.0;
    }
    return -1.0;
  }

  private func EncodeColor(color: HDRColor) -> ref<JsonArray> {
    let values = new JsonArray();
    let channels = [color.Red, color.Green, color.Blue];
    // Match the picker, whose range extends beyond 255 for HDR.
    for component in channels {
      values.AddItemInt64(Cast<Int64>(ClampF(component * 255.0, 0.0, 400.0) + 0.5));
    }
    return values;
  }

  public func OpenMenu(controller: wref<inkGameController>) -> Void {
    if NotEquals(this.menuController, controller) {
      this.section = null;
    }
    this.menuController = controller;
    let root = controller.GetRootCompoundWidget();
    if IsDefined(root) && !IsDefined(root.GetWidgetByPathName(n"ChromeAxisPresetEvents")) {
      let events = new ChromeAxisHudPainterPresetEvents();
      events.Reparent(root);
    }
    this.RefreshMenu();
  }

  private cb func OnHudPainterWidgetSpawn(event: ref<inkWidgetSpawnEvent>) {
    let instance = event.GetItemInstance();
    if IsDefined(instance) {
      let picker = instance.gameController as CustomColorPickerController;
      if IsDefined(picker) {
        this.picker = picker;
      }
    }
    // HUD Painter recreates the picker after rebuilding its color list.
    this.RefreshMenu();
  }

  private func RefreshMenu() -> Void {
    this.SyncActivePreset();
    this.MountSection();
  }

  private func MountSection() -> Void {
    if !IsDefined(this.menuController) {
      return;
    }
    let root = this.menuController.GetRootCompoundWidget();
    let list = this.FindColorList(root);
    if !IsDefined(list) || IsDefined(list.GetWidgetByPathName(n"ChromeAxisColors")) {
      return;
    }
    // HUD Painter serializes direct color-row children. Our rows live in this subpanel.
    let section = new ChromeAxisHudPainterSection();
    section.Reparent(list, 0);
  }

  private func FindColorList(root: wref<inkCompoundWidget>) -> wref<inkCompoundWidget> {
    if !IsDefined(root) {
      return null;
    }
    let index = 0;
    while index < root.GetNumChildren() {
      let child = root.GetWidgetByIndex(index);
      if Equals(child.GetName(), n"ChromeAxisColors") || IsDefined(child.GetController() as ComponentColorItem) {
        return root;
      }
      let list = this.FindColorList(child as inkCompoundWidget);
      if IsDefined(list) {
        return list;
      }
      index += 1;
    }
    return null;
  }
}

@if(ModuleExists("HudPainter"))
class ChromeAxisHudPainterMenuReady extends Event {}

@if(ModuleExists("HudPainter"))
@wrapMethod(gameuiMenuGameController)
protected cb func OnSetMenuEventDispatcher(menuEventDispatcher: wref<inkMenuEventDispatcher>) -> Bool {
  let result = wrappedMethod(menuEventDispatcher);
  if this.IsA(n"HudPainter.HudPainterController") {
    this.ChromeAxisMountHudPainterSection();
    // The dispatcher may be set before the existing color rows are initialized.
    this.QueueEvent(new ChromeAxisHudPainterMenuReady());
  }
  return result;
}

@if(ModuleExists("HudPainter"))
@addMethod(gameuiMenuGameController)
private func ChromeAxisMountHudPainterSection() -> Void {
  let menu = ChromeAxisHudPainterMenu.Get();
  if IsDefined(menu) {
    menu.OpenMenu(this);
  }
}

@if(ModuleExists("HudPainter"))
@addMethod(gameuiMenuGameController)
protected cb func OnChromeAxisHudPainterMenuReady(evt: ref<ChromeAxisHudPainterMenuReady>) -> Bool {
  if this.IsA(n"HudPainter.HudPainterController") {
    this.ChromeAxisMountHudPainterSection();
  }
  return false;
}

@if(ModuleExists("HudPainter"))
class ChromeAxisHudColorFlush extends DelayCallback {
  public let menu: wref<ChromeAxisHudPainterMenu>;

  public func Call() -> Void {
    if IsDefined(this.menu) {
      this.menu.FlushAfterInput(this);
    }
  }
}

@if(ModuleExists("HudPainter"))
private func ChromeAxis_SetPickerChannels(picker: wref<CustomColorPickerController>, color: HDRColor) -> Void {
  // Seed all channels before HUD Painter handles the updates emitted by slider Setup.
  let type = Reflection.GetClassOf(picker);
  if IsDefined(type) {
    let red = type.GetProperty(n"sliderValueR");
    let green = type.GetProperty(n"sliderValueG");
    let blue = type.GetProperty(n"sliderValueB");
    if IsDefined(red) && IsDefined(green) && IsDefined(blue) {
      red.SetValue(picker, ChromeAxis_HudSliderValue(color.Red));
      green.SetValue(picker, ChromeAxis_HudSliderValue(color.Green));
      blue.SetValue(picker, ChromeAxis_HudSliderValue(color.Blue));
    }
  }
}

@if(ModuleExists("HudPainter"))
@addMethod(inkGameController)
protected cb func OnChromeAxisHudPresetWriteBoundary(event: ref<ChromeAxisHudPresetWriteBoundary>) -> Bool {
  let menu = ChromeAxisHudPainterMenu.Get();
  if IsDefined(menu) {
    menu.ObservePresetWriteBoundary(event);
  }
  return false;
}

// Game controllers can receive the queued result after the HUD Painter view has detached.
@if(ModuleExists("HudPainter"))
@addMethod(inkGameController)
protected cb func OnHudPainterPresetSaved(event: ref<HudPainterPresetSaved>) -> Bool {
  let menu = ChromeAxisHudPainterMenu.Get();
  if IsDefined(menu) {
    menu.PresetSaved(event);
  }
  return false;
}

@if(ModuleExists("HudPainter"))
class ChromeAxisHudPainterPresetEvents extends inkComponent {
  protected cb func OnCreate() -> ref<inkWidget> {
    let widget = new inkCanvas();
    widget.SetName(n"ChromeAxisPresetEvents");
    widget.SetInteractive(false);
    return widget;
  }

  protected cb func OnHudPainterPresetSaved(event: ref<HudPainterPresetSaved>) -> Bool {
    let menu = ChromeAxisHudPainterMenu.Get();
    if IsDefined(menu) {
      menu.PresetSaved(event);
    }
    return false;
  }

  protected cb func OnChromeAxisHudPresetWriteBoundary(event: ref<ChromeAxisHudPresetWriteBoundary>) -> Bool {
    let menu = ChromeAxisHudPainterMenu.Get();
    if IsDefined(menu) {
      menu.ObservePresetWriteBoundary(event);
    }
    return false;
  }

  protected cb func OnHudPainterInkStyleRefreshed(event: ref<HudPainterInkStyleRefreshed>) -> Bool {
    let menu = ChromeAxisHudPainterMenu.Get();
    if IsDefined(menu) {
      menu.SyncActivePreset();
    }
    return false;
  }
}

@if(ModuleExists("HudPainter"))
@wrapMethod(GenericMessageNotification)
private final func Close(result: GenericMessageNotificationResult) -> Void {
  let saveMenu: ref<ChromeAxisHudPainterMenu>;
  let presetWrite: ref<ChromeAxisHudPresetWrite>;
  if Equals(result, GenericMessageNotificationResult.Confirm)
    && IsDefined(this.m_data) && this.m_data.isInput && IsDefined(this.m_textInput)
    && Equals(this.m_data.title, GetLocalizedTextByKey(n"Mod-HudPainter-Save-Preset-Title")) {
    let name = this.m_textInput.GetText();
    if NotEquals(name, "") && NotEquals(StrLower(name), "default") {
      saveMenu = ChromeAxisHudPainterMenu.Get();
      if IsDefined(saveMenu) {
        presetWrite = saveMenu.BeginPresetSave(name);
      }
    }
  }
  wrappedMethod(result);
  if IsDefined(presetWrite) {
    saveMenu.FinishPresetSave(presetWrite);
  }
}

@if(ModuleExists("HudPainter"))
class ChromeAxisHudPainterSection extends inkComponent {
  private let french: Bool;
  private let border: wref<ComponentColorItem>;
  private let transmission: wref<ComponentColorItem>;
  private let selectedColor: ref<HudPainterColorItem>;
  private let message: wref<inkText>;

  protected cb func OnCreate() -> ref<inkWidget> {
    let panel = new inkVerticalPanel();
    panel.SetName(n"ChromeAxisColors");
    panel.SetFitToContent(true);
    panel.SetMargin(0.0, 8.0, 0.0, 20.0);
    return panel;
  }

  protected cb func OnInitialize() {
    let panel = this.GetRootCompoundWidget();
    this.french = ChromeAxis_IsFrenchInterface(GetGameInstance());
    let defaults = ChromeAxisHudPalette.Default();
    let palette = ChromeAxis_LoadHudPalette();

    let heading = this.AddLabel(panel, "Chrome Axis", 32);
    heading.SetMargin(32.0, 8.0, 0.0, 2.0);
    heading.SetStyle(r"base\\gameplay\\gui\\common\\main_colors.inkstyle");
    heading.BindProperty(n"tintColor", n"MainColors.Blue");
    this.border = this.AddColor(panel, this.french ? "Contour du badge" : "Badge outline", defaults.border, palette.border);
    this.transmission = this.AddColor(panel, this.french ? "Texte de la boîte" : "Gearbox text", defaults.transmission, palette.transmission);

    this.message = this.AddLabel(panel, this.SaveLabel(ChromeAxisHudSaveStatus.Available), 24);
    this.message.SetMargin(32.0, 2.0, 0.0, 0.0);
    let menu = ChromeAxisHudPainterMenu.Get();
    if IsDefined(menu) {
      menu.AttachSection(this);
    }
  }

  protected cb func OnUninitialize() {
    let menu = ChromeAxisHudPainterMenu.Get();
    if IsDefined(menu) {
      menu.DetachSection(this);
    }
  }

  protected cb func OnHudPainterColorSelected(event: ref<HudPainterColorSelected>) -> Bool {
    this.selectedColor = event.data;
    if this.HasColor(event.data) {
      let menu = ChromeAxisHudPainterMenu.Get();
      if IsDefined(menu) {
        menu.PreparePicker(event.data);
      }
    }
    return false;
  }

  protected cb func OnHudPainterColorChanged(event: ref<HudPainterColorChanged>) -> Bool {
    if !IsDefined(this.border) || !IsDefined(this.transmission) || !IsDefined(this.selectedColor)
      || !Equals(event.type, HudPainterColorType.Default) || !Equals(event.name, this.selectedColor.name) {
      return false;
    }
    let isBorder = Equals(this.selectedColor, this.border.GetData());
    if isBorder || Equals(this.selectedColor, this.transmission.GetData()) {
      let menu = ChromeAxisHudPainterMenu.Get();
      if IsDefined(menu) {
        menu.ChangeColor(isBorder, event.color);
      }
    }
    return false;
  }

  protected cb func OnHudPainterColorPreviewAvailable(event: ref<HudPainterColorPreviewAvailable>) -> Bool {
    // HUD Painter emits previews when selecting a row as well as when releasing a slider.
    if this.HasColor(event.color) {
      let menu = ChromeAxisHudPainterMenu.Get();
      if IsDefined(menu) {
        menu.FlushColors();
      }
    }
    return false;
  }

  public func HasColor(color: ref<HudPainterColorItem>) -> Bool {
    return IsDefined(color) && ((IsDefined(this.border) && Equals(color, this.border.GetData()))
      || (IsDefined(this.transmission) && Equals(color, this.transmission.GetData())));
  }

  public func SyncColors(saved: ref<ChromeAxisHudPalette>, current: ref<ChromeAxisHudPalette>,
    status: ChromeAxisHudSaveStatus, picker: wref<CustomColorPickerController>) -> Void {
    this.SyncItem(this.border, saved.border, current.border, picker);
    this.SyncItem(this.transmission, saved.transmission, current.transmission, picker);
    this.message.SetText(this.SaveLabel(status));
  }

  private func SyncItem(item: wref<ComponentColorItem>, saved: HDRColor, current: HDRColor,
    picker: wref<CustomColorPickerController>) -> Void {
    let color = item.GetData();
    if ChromeAxis_HudColorsMatch(color.presetColor, saved) && ChromeAxis_HudColorsMatch(color.customColor, current) {
      return;
    }
    color.presetColor = saved;
    color.customColor = current;
    item.RefreshItemData();
    if IsDefined(picker) && Equals(color, this.selectedColor) {
      // Invoke the picker's selection callback locally. Preview events never create edits.
      ChromeAxis_SetPickerChannels(picker, current);
      Reflection.Call(picker, n"OnHudPainterColorSelected", [ToVariant(HudPainterColorSelected.Create(color))]);
    }
  }

  private func SaveLabel(status: ChromeAxisHudSaveStatus) -> String {
    switch status {
      case ChromeAxisHudSaveStatus.FileUnavailable:
        return this.french ? "Lecture ou écriture des couleurs impossible · Modifications en attente" :
          "Could not read or write colors · Changes pending";
      case ChromeAxisHudSaveStatus.RetryingFile:
        return this.french ? "Fichier des couleurs indisponible · Nouvelle tentative automatique" :
          "Color file unavailable · Retrying automatically";
      case ChromeAxisHudSaveStatus.InvalidFile:
        return this.french ? "Fichier des couleurs malformé · Modifications en attente" :
          "Malformed color file · Changes pending";
      case ChromeAxisHudSaveStatus.EmptyFile:
        return this.french ? "Fichier des couleurs vide · Modifications en attente" :
          "Empty color file · Changes pending";
      case ChromeAxisHudSaveStatus.FileTooLarge:
        return this.french ? "Fichier des couleurs supérieur à 4 Mio · Modifications en attente" :
          "Color file exceeds 4 MiB · Changes pending";
      case ChromeAxisHudSaveStatus.ReloadingExternalEdit:
        return this.french ? "Fichier modifié ailleurs · Relecture automatique" :
          "File changed elsewhere · Reloading automatically";
      case ChromeAxisHudSaveStatus.ExternalEditPending:
        return this.french ? "Fichier en conflit · Modifications en attente" :
          "Color file conflict · Changes pending";
      case ChromeAxisHudSaveStatus.RecoveryRequired:
        return this.french ? "Récupération requise · Vérifiez colors.json.recovery*" :
          "Recovery required · Check colors.json.recovery*";
      case ChromeAxisHudSaveStatus.RecoveryScanDenied:
        return this.french ? "Couleurs en lecture seule · Dossier de récupération inaccessible" :
          "Colors are read-only · Recovery folder listing denied";
      case ChromeAxisHudSaveStatus.InvalidPreset:
        return this.french ? "Préréglage Chrome Axis illisible · Couleurs non enregistrables" :
          "Chrome Axis preset unreadable · Colors cannot be saved";
      case ChromeAxisHudSaveStatus.OtherPresetPending:
        return this.french ? "Enregistrement automatique · Un autre préréglage reste en attente" :
          "Autosave enabled · Another preset is still pending";
    }
    return this.french ? "Enregistrement automatique" : "Autosave enabled";
  }

  private func AddColor(parent: ref<inkCompoundWidget>, label: String, defaultColor: HDRColor, savedColor: HDRColor) -> ref<ComponentColorItem> {
    let color = new HudPainterColorItem();
    color.name = label;
    color.type = HudPainterColorType.Default;
    color.defaultColor = defaultColor;
    color.presetColor = savedColor;
    color.customColor = savedColor;
    let item = new ComponentColorItem();
    item.Reparent(parent);
    item.SetData(color);
    return item;
  }

  private func AddLabel(parent: ref<inkCompoundWidget>, text: String, size: Int32) -> ref<inkText> {
    let label = new inkText();
    label.SetFontFamily("base\\gameplay\\gui\\fonts\\raj\\raj.inkfontfamily");
    label.SetFontStyle(n"Bold");
    label.SetFontSize(size);
    label.SetText(text);
    label.SetInteractive(false);
    label.Reparent(parent);
    return label;
  }
}

public static native func ChromeAxis_DriverHeartbeat(vehicle: ref<VehicleObject>, canDrive: Bool, forward: Vector4) -> Int32;
public static native func ChromeAxis_CaptureEnabled() -> Bool;
public static native func ChromeAxis_CaptureVehicleRecord(vehicle: ref<VehicleObject>, record: TweakDBID) -> Void;
public static native func ChromeAxis_TransmissionState(vehicle: ref<VehicleObject>) -> Int32;
public static native func ChromeAxis_FeedbackEnabled() -> Bool;
public static native func ChromeAxis_FeedbackMotion(vehicle: ref<VehicleObject>, velocity: Vector4, forward: Vector4, right: Vector4, mass: Float, simTime: Float, skidding: Bool, airborne: Bool) -> Void;
public static native func ChromeAxis_VehicleImpact(vehicle: ref<VehicleObject>, velocityChange: Float, lateral: Float) -> Void;
@if(ModuleExists("HudPainter"))
public static native func ChromeAxis_CommitHudColors(json: String, previousJson: String, existed: Bool) -> Int32;
@if(ModuleExists("HudPainter"))
public static native func ChromeAxis_RecoverHudColors(probeReadable: Bool) -> Int32;

public class ChromeAxisDrivingContext extends ScriptableSystem {
  private let player: wref<PlayerPuppet>;
  private let generation: Int32;
  private let driverVehicle: wref<VehicleObject>;
  private let feedbackPolling: Bool;
  private let feedbackGeneration: Int32;
  private let hudBadge: wref<inkCanvas>;
  private let hudBorder: array<wref<inkRectangle>>;
  private let hudDivider: wref<inkRectangle>;
  private let hudLogo: wref<inkImage>;
  private let hudInnerFrame: wref<inkImage>;
  private let hudLogoFrame: wref<inkImage>;
  private let hudGraphicsReady: Bool;
  private let hudGraphicsRetries: Int32;
  private let hudLabel: wref<inkText>;
  private let hudLabelShadow: wref<inkText>;
  private let hudGearLabel: wref<inkText>;
  private let hudGearCaption: wref<inkText>;
  private let hudModeLabel: wref<inkText>;
  private let hudGear: Int32;
  private let hudManualMode: Bool;
  private let hudStatus: Int32;
  private let hudRetry: Int32;
  private let hudFrench: Bool;

  private func OnPlayerAttach(request: ref<PlayerAttachRequest>) -> Void {
    if !IsDefined(request) {
      return;
    }
    let player = request.owner as PlayerPuppet;
    if !IsDefined(player) {
      return;
    }
    this.Clear();
    this.player = player;
    this.Refresh(this.generation);
  }

  private func OnPlayerDetach(request: ref<PlayerDetachRequest>) -> Void {
    let player: ref<PlayerPuppet> = this.player;
    if IsDefined(request) && EntityID.IsDefined(request.ownerID) && IsDefined(player) &&
       !Equals(request.ownerID, player.GetEntityID()) {
      return;
    }
    this.Clear();
  }

  public func OnDetach() -> Void {
    this.Clear();
  }

  private func Clear() -> Void {
    this.player = null;
    this.generation += 1;
    this.driverVehicle = null;
    this.feedbackPolling = false;
    this.feedbackGeneration += 1;
    ChromeAxis_DriverHeartbeat(null, false, new Vector4(0.0, 0.0, 0.0, 0.0));
    let badge: ref<inkCanvas> = this.hudBadge;
    if IsDefined(badge) {
      let parent = badge.GetParentWidget() as inkCompoundWidget;
      if IsDefined(parent) {
        parent.RemoveChild(badge);
      }
    }
    this.hudBadge = null;
    ArrayClear(this.hudBorder);
    this.hudDivider = null;
    this.hudLogo = null;
    this.hudInnerFrame = null;
    this.hudLogoFrame = null;
    this.hudGraphicsReady = false;
    this.hudGraphicsRetries = 0;
    this.hudLabel = null;
    this.hudLabelShadow = null;
    this.hudGearLabel = null;
    this.hudGearCaption = null;
    this.hudModeLabel = null;
    this.hudGear = -2;
    this.hudManualMode = false;
    this.hudStatus = 0;
    this.hudRetry = 0;
    this.hudFrench = false;
  }

  public func AttachHud(root: ref<inkCompoundWidget>, generation: Int32) -> Bool {
    if generation != this.generation || this.hudStatus == 0 || !IsDefined(root) {
      return false;
    }
    let rootWidget: wref<inkWidget> = root;
    let ancestor = rootWidget;
    while IsDefined(ancestor) {
      if !ancestor.IsVisible() {
        return false;
      }
      ancestor = ancestor.GetParentWidget();
    }

    let existingBadge: ref<inkCanvas> = this.hudBadge;
    if IsDefined(existingBadge) {
      if NotEquals(existingBadge.GetParentWidget(), rootWidget) {
        existingBadge.Reparent(root);
        this.ApplyHudPalette();
        this.ApplyHudStatus();
      }
      return true;
    }

    let badge = new inkCanvas();
    badge.SetName(n"ChromeAxisStatusBadge");
    badge.SetSize(Vector2(236.0, 60.0));
    badge.SetAnchor(inkEAnchor.TopLeft);
    badge.SetAnchorPoint(0.0, 0.0);
    badge.SetTranslation(36.0, 176.0);
    badge.SetInteractive(false);
    badge.Reparent(root);
    this.BuildHudFrame(badge);

    let logo = new inkImage();
    logo.SetVisible(false);
    logo.SetInteractive(false);
    logo.SetAtlasResource(r"chrome_axis\\hud\\logo.inkatlas");
    logo.SetSize(Vector2(20.0, 20.0));
    logo.SetTranslation(16.0, 19.0);
    logo.Reparent(badge);

    let shadow = this.CreateHudLabel(badge, 43.0, 12.0, 130.0, 20.0, 17, textHorizontalAlignment.Left);
    shadow.SetTintColor(HDRColor(0.0, 0.0, 0.0, 0.85));
    let label = this.CreateHudLabel(badge, 42.0, 11.0, 130.0, 20.0, 17, textHorizontalAlignment.Left);

    let modeLabel = this.CreateHudLabel(badge, 42.0, 32.0, 130.0, 16.0, 13, textHorizontalAlignment.Left);
    let gearColumn = new inkCanvas();
    gearColumn.SetInteractive(false);
    gearColumn.SetAnchor(inkEAnchor.TopLeft);
    gearColumn.SetAnchorPoint(0.0, 0.0);
    gearColumn.SetSize(Vector2(44.0, 40.0));
    gearColumn.SetTranslation(176.0, 10.0);
    gearColumn.Reparent(badge);
    let gearCaption = this.CreateCenteredHudLabel(gearColumn, 4.0, 13.0, 9);
    let gearLabel = this.CreateCenteredHudLabel(gearColumn, 15.0, 23.0, 25);

    this.hudBadge = badge;
    this.hudLogo = logo;
    this.hudGraphicsReady = false;
    this.hudGraphicsRetries = 30;
    this.hudLabel = label;
    this.hudLabelShadow = shadow;
    this.hudGearLabel = gearLabel;
    this.hudGearCaption = gearCaption;
    this.hudModeLabel = modeLabel;
    this.ApplyHudPalette();
    this.ApplyHudStatus();
    this.ApplyHudGear();
    this.TryShowHudGraphics();
    return true;
  }

  private func TryShowHudGraphics() -> Void {
    if this.hudGraphicsReady || this.hudGraphicsRetries <= 0 {
      return;
    }
    this.hudGraphicsRetries -= 1;
    let logoReady = this.TryShowHudImage(this.hudLogo, n"logo");
    let frameReady = this.TryShowHudImage(this.hudInnerFrame, n"status_cell_fg");
    let cornersReady = this.TryShowHudImage(this.hudLogoFrame, n"item_grid_frame");
    this.hudGraphicsReady = logoReady && frameReady && cornersReady;
  }

  private func TryShowHudImage(image: wref<inkImage>, part: CName) -> Bool {
    if !IsDefined(image) {
      return false;
    }
    if image.IsVisible() {
      return true;
    }
    // Ink can show a solid placeholder while the atlas loads.
    if !image.IsTexturePartExist(part) || !image.SetTexturePart(part) {
      return false;
    }
    image.SetVisible(true);
    return true;
  }

  private func BuildHudFrame(badge: ref<inkCanvas>) -> Void {
    this.ApplyMinimapBackground(this.CreateHudRect(badge, 0.0, 2.0, 227.0, 56.0));
    this.ApplyMinimapBackground(this.CreateHudRect(badge, 227.0, 2.0, 9.0, 48.0));
    this.hudInnerFrame = this.CreateHudDetail(badge, Vector2(5.0, 7.0), Vector2(217.0, 46.0), 0.44, true);
    this.hudLogoFrame = this.CreateHudDetail(badge, Vector2(15.0, 18.0), Vector2(22.0, 22.0), 0.38, false);
    ArrayClear(this.hudBorder);
    ArrayPush(this.hudBorder, this.CreateHudRect(badge, 0.0, 2.0, 236.0, 1.0));
    ArrayPush(this.hudBorder, this.CreateHudRect(badge, 0.0, 3.0, 1.0, 54.0));
    ArrayPush(this.hudBorder, this.CreateHudRect(badge, 0.0, 57.0, 228.0, 1.0));
    ArrayPush(this.hudBorder, this.CreateHudRect(badge, 235.0, 3.0, 1.0, 46.0));
    ArrayPush(this.hudBorder, this.CreateHudRect(badge, 227.0, 49.0, 9.0, 1.0));
    ArrayPush(this.hudBorder, this.CreateHudRect(badge, 227.0, 50.0, 1.0, 7.0));
    ArrayPush(this.hudBorder, this.CreateHudRect(badge, 12.0, 0.0, 16.0, 2.0));
    this.hudDivider = this.CreateHudRect(badge, 173.0, 12.0, 1.0, 36.0);
  }

  private func ApplyMinimapBackground(panel: ref<inkRectangle>) -> Void {
    panel.SetTintColor(HDRColor(14.0 / 255.0, 14.0 / 255.0, 23.0 / 255.0, 1.0));
    panel.SetOpacity(0.75);
    panel.SetStyle(r"base\\gameplay\\gui\\common\\main_colors.inkstyle");
    panel.BindProperty(n"opacity", n"MiniMapStyle.bgOpacity");
  }

  private func CreateHudDetail(badge: ref<inkCanvas>, position: Vector2, size: Vector2, opacity: Float, nineSlice: Bool) -> ref<inkImage> {
    let detail = new inkImage();
    detail.SetVisible(false);
    detail.SetInteractive(false);
    detail.SetAtlasResource(r"base\\gameplay\\gui\\common\\shapes\\atlas_shapes_sync.inkatlas");
    detail.useNineSliceScale = nineSlice;
    detail.SetBrushTileType(inkBrushTileType.NoTile);
    detail.SetContentHAlign(inkEHorizontalAlign.Fill);
    detail.SetContentVAlign(inkEVerticalAlign.Fill);
    detail.SetTintColor(HDRColor(0.48, 0.53, 0.59, 1.0));
    detail.SetOpacity(opacity);
    detail.SetSize(size);
    detail.SetTranslation(position);
    detail.Reparent(badge);
    return detail;
  }

  public func ApplyHudPalette() -> Void {
    if !IsDefined(this.hudBadge) {
      return;
    }
    let palette = ChromeAxis_LoadHudPalette();
    for line in this.hudBorder {
      this.ApplyHudTint(line, palette.border, 0.86);
    }
    this.ApplyHudTint(this.hudDivider, palette.transmission, 0.45);
    this.ApplyHudTint(this.hudModeLabel, palette.transmission, 1.0);
    this.ApplyHudTint(this.hudGearCaption, palette.transmission, 1.0);
    this.ApplyHudTint(this.hudGearLabel, palette.transmission, 1.0);
  }

  private func ApplyHudTint(widget: wref<inkWidget>, color: HDRColor, opacity: Float) -> Void {
    if IsDefined(widget) {
      widget.SetTintColor(color);
      widget.SetOpacity(opacity);
    }
  }

  private func CreateHudRect(badge: ref<inkCanvas>, x: Float, y: Float, width: Float, height: Float) -> ref<inkRectangle> {
    let rectangle = new inkRectangle();
    rectangle.SetInteractive(false);
    rectangle.SetSize(Vector2(width, height));
    rectangle.SetTranslation(x, y);
    rectangle.Reparent(badge);
    return rectangle;
  }

  private func CreateHudLabel(badge: ref<inkCanvas>, x: Float, y: Float, width: Float, height: Float, fontSize: Int32, alignment: textHorizontalAlignment) -> ref<inkText> {
    let label = new inkText();
    label.SetInteractive(false);
    label.SetFontFamily("base\\gameplay\\gui\\fonts\\raj\\raj.inkfontfamily");
    label.SetFontStyle(n"Bold");
    label.SetFontSize(fontSize);
    label.SetHorizontalAlignment(alignment);
    label.SetVerticalAlignment(textVerticalAlignment.Center);
    label.SetSize(Vector2(width, height));
    label.SetTranslation(x, y);
    label.Reparent(badge);
    return label;
  }

  private func CreateCenteredHudLabel(column: ref<inkCanvas>, y: Float, height: Float, fontSize: Int32) -> ref<inkText> {
    let label = this.CreateHudLabel(column, 0.0, y, 44.0, height, fontSize, textHorizontalAlignment.Left);
    label.SetFitToContent(true);
    label.SetAnchor(inkEAnchor.TopCenter);
    label.SetAnchorPoint(0.5, 0.0);
    return label;
  }

  private func ReadHudGear(vehicle: wref<VehicleObject>, nativeGear: Int32) -> Int32 {
    if nativeGear >= -1 && nativeGear <= 8 {
      return nativeGear;
    }

    let board = vehicle.GetBlackboard();
    if IsDefined(board) {
      let gear = board.GetInt(GetAllBlackboardDefs().Vehicle.GearValue);
      if gear >= 1 && gear <= 8 {
        return gear;
      }
    }
    return -2;
  }

  private func ApplyHudGear() -> Void {
    if !IsDefined(this.hudGearLabel) || !IsDefined(this.hudGearCaption) || !IsDefined(this.hudModeLabel) {
      return;
    }
    let value = this.hudGear > 0 ? IntToString(this.hudGear) : (this.hudGear == 0 ? "R" : (this.hudGear == -1 ? "N" : "—"));
    let mode = this.hudManualMode ? (this.hudFrench ? "MANUEL" : "MANUAL") : "AUTO";
    this.hudGearCaption.SetText(this.hudFrench ? "RAPPORT" : "GEAR");
    this.hudGearLabel.SetText(value);
    this.hudModeLabel.SetText(mode);
  }

  private func UpdateHud(game: GameInstance, status: Int32, vehicle: wref<VehicleObject>) -> Void {
    if status != 0 && this.hudStatus == 0 {
      this.hudFrench = ChromeAxis_IsFrenchInterface(game);
    }
    let gear: Int32 = -2;
    let manualMode: Bool = false;
    if status != 0 && IsDefined(vehicle) {
      let transmission = ChromeAxis_TransmissionState(vehicle);
      manualMode = transmission >= 16;
      gear = this.ReadHudGear(vehicle, (manualMode ? transmission - 16 : transmission) - 2);
    }
    let modeChanged = this.hudManualMode ? !manualMode : manualMode;
    let gearLineChanged = this.hudGear != gear || modeChanged;
    this.hudGear = gear;
    this.hudManualMode = manualMode;
    let changed = this.hudStatus != status;
    this.hudStatus = status;
    if status == 0 {
      if changed && IsDefined(this.hudBadge) {
        this.hudBadge.SetVisible(false);
      }
      this.hudRetry = 0;
      return;
    }
    if IsDefined(this.hudBadge) {
      if changed {
        this.ApplyHudStatus();
        if !this.hudGraphicsReady {
          this.hudGraphicsRetries = 30;
        }
      }
      if changed || gearLineChanged {
        this.ApplyHudGear();
      }
      this.TryShowHudGraphics();
    }
    if this.hudRetry > 0 {
      this.hudRetry -= 1;
      return;
    }
    // The game may replace the HUD root while the previous badge is still alive.
    this.hudRetry = 9;
    let ui = GameInstance.GetUISystem(game);
    if IsDefined(ui) {
      let event = new ChromeAxisAttachHudEvent();
      event.context = this;
      event.generation = this.generation;
      ui.QueueEvent(event);
    }
  }

  private func ApplyHudStatus() -> Void {
    if !IsDefined(this.hudLabel) || !IsDefined(this.hudBadge) {
      return;
    }
    let text: String;
    let color: HDRColor;
    switch this.hudStatus {
      case 1:
        text = this.hudFrench ? "MOD CHARGÉ" : "MOD LOADED";
        color = HDRColor(0.77, 0.81, 0.87, 1.0);
        break;
      case 2:
        text = this.hudFrench ? "USB EN ATTENTE" : "USB WAITING";
        color = HDRColor(1.0, 0.84, 0.18, 1.0);
        break;
      case 3:
        text = this.hudFrench ? "USB PRÊT" : "USB READY";
        color = HDRColor(0.40, 0.95, 1.0, 1.0);
        break;
      case 4:
        text = this.hudFrench ? "SIGNAL USB" : "USB INPUT";
        color = HDRColor(0.46, 1.0, 0.62, 1.0);
        break;
      case 5:
        text = "SIMULATION";
        color = HDRColor(0.74, 0.61, 1.0, 1.0);
        break;
      case 6:
        text = this.hudFrench ? "USB PARTIEL" : "PARTIAL USB";
        color = HDRColor(1.0, 0.68, 0.25, 1.0);
        break;
      default:
        return;
    }
    this.hudLabel.SetText(text);
    this.hudLabel.SetTintColor(color);
    if IsDefined(this.hudLogo) {
      this.hudLogo.SetTintColor(color);
    }
    if IsDefined(this.hudLabelShadow) {
      this.hudLabelShadow.SetText(text);
    }
    this.hudBadge.SetVisible(true);
  }

  private func CanDrive(game: GameInstance, player: ref<PlayerPuppet>, out paused: Bool) -> Bool {
    paused = false;
    let boards: ref<BlackboardSystem> = GameInstance.GetBlackboardSystem(game);
    if !IsDefined(boards) {
      return false;
    }
    let ui: ref<IBlackboard> = boards.Get(GetAllBlackboardDefs().UI_System);
    if !IsDefined(ui) {
      return false;
    }
    let photoMode: ref<IBlackboard> = boards.Get(GetAllBlackboardDefs().PhotoMode);
    paused = ui.GetBool(GetAllBlackboardDefs().UI_System.IsInMenu) ||
             (IsDefined(photoMode) && photoMode.GetBool(GetAllBlackboardDefs().PhotoMode.IsActive));
    if paused {
      return false;
    }
    let stateBoard: ref<IBlackboard> = player.GetPlayerStateMachineBlackboard();
    if !IsDefined(stateBoard) {
      return false;
    }
    let state: gamePSMVehicle = IntEnum<gamePSMVehicle>(stateBoard.GetInt(GetAllBlackboardDefs().PlayerStateMachine.Vehicle));
    return Equals(state, gamePSMVehicle.Driving) || Equals(state, gamePSMVehicle.DriverCombat);
  }

  public func Refresh(generation: Int32) -> Void {
    let player: ref<PlayerPuppet> = this.player;
    if generation != this.generation || !IsDefined(player) {
      return;
    }
    let game: GameInstance = player.GetGame();
    let paused: Bool;
    let canDrive = this.CanDrive(game, player, paused);
    let mounted: wref<VehicleObject> = this.driverVehicle;
    if canDrive {
      if !(VehicleComponent.GetVehicle(game, player.GetEntityID(), mounted) &&
           IsDefined(mounted) && VehicleComponent.IsDriver(game, player)) {
        mounted = null;
      }
    }
    let vehicle: ref<VehicleObject> = mounted;
    if !canDrive && (!paused || !IsDefined(vehicle) || !vehicle.IsPlayerDriver()) {
      vehicle = null;
    }
    let forward: Vector4;
    if canDrive && IsDefined(vehicle) {
      forward = vehicle.GetWorldForward();
    }
    let currentMount: wref<VehicleObject> = vehicle;
    if NotEquals(this.driverVehicle, currentMount) {
      this.feedbackGeneration += 1;
      this.feedbackPolling = false;
    }
    let status = ChromeAxis_DriverHeartbeat(vehicle, canDrive, forward);
    this.driverVehicle = currentMount;
    if !canDrive {
      vehicle = null;
    }
    if IsDefined(vehicle) && ChromeAxis_CaptureEnabled() {
      ChromeAxis_CaptureVehicleRecord(vehicle, vehicle.GetRecordID());
    }
    if IsDefined(vehicle) && !this.feedbackPolling && ChromeAxis_FeedbackEnabled() {
      this.feedbackPolling = true;
      this.RefreshFeedback(generation, this.feedbackGeneration);
    }

    let next: ref<ChromeAxisDrivingTick> = new ChromeAxisDrivingTick();
    next.context = this;
    next.generation = generation;
    GameInstance.GetDelaySystem(game).DelayCallback(next, 0.10, false);
    this.UpdateHud(game, status, vehicle);
  }

  public func RefreshFeedback(generation: Int32, feedbackGeneration: Int32) -> Void {
    let player: ref<PlayerPuppet> = this.player;
    if generation != this.generation || feedbackGeneration != this.feedbackGeneration || !IsDefined(player) {
      return;
    }
    if !ChromeAxis_FeedbackEnabled() {
      this.feedbackPolling = false;
      return;
    }
    let game: GameInstance = player.GetGame();
    let vehicle: ref<VehicleObject> = this.driverVehicle;
    if !IsDefined(vehicle) || !vehicle.IsPlayerDriver() {
      vehicle = null;
    }
    let paused: Bool;
    if !IsDefined(vehicle) || !this.CanDrive(game, player, paused) {
      this.feedbackPolling = false;
      if !paused {
        vehicle = null;
      }
      ChromeAxis_DriverHeartbeat(vehicle, false, new Vector4(0.0, 0.0, 0.0, 0.0));
      return;
    }

    let airborne: Bool = vehicle.IsInAir();
    ChromeAxis_FeedbackMotion(vehicle, vehicle.GetLinearVelocity(), vehicle.GetWorldForward(), vehicle.GetWorldRight(),
      vehicle.GetTotalMass(), EngineTime.ToFloat(GameInstance.GetSimTime(game)), !airborne && vehicle.IsSkidding(2.5), airborne);

    let next: ref<ChromeAxisFeedbackTick> = new ChromeAxisFeedbackTick();
    next.context = this;
    next.generation = generation;
    next.feedbackGeneration = feedbackGeneration;
    GameInstance.GetDelaySystem(game).DelayCallback(next, 0.04, false);
  }
}

public class ChromeAxisDrivingTick extends DelayCallback {
  public let context: wref<ChromeAxisDrivingContext>;
  public let generation: Int32;

  public func Call() -> Void {
    if IsDefined(this.context) {
      this.context.Refresh(this.generation);
    }
  }
}

public class ChromeAxisFeedbackTick extends DelayCallback {
  public let context: wref<ChromeAxisDrivingContext>;
  public let generation: Int32;
  public let feedbackGeneration: Int32;

  public func Call() -> Void {
    if IsDefined(this.context) {
      this.context.RefreshFeedback(this.generation, this.feedbackGeneration);
    }
  }
}

public class ChromeAxisAttachHudEvent extends Event {
  public let context: wref<ChromeAxisDrivingContext>;
  public let generation: Int32;
  public let attached: Bool;
}

@addMethod(inkGameController)
protected cb func OnChromeAxisAttachHudEvent(evt: ref<ChromeAxisAttachHudEvent>) -> Bool {
  if IsDefined(evt) && this.IsA(n"gameuiRootHudGameController") && !evt.attached && IsDefined(evt.context) {
    evt.attached = evt.context.AttachHud(this.GetRootCompoundWidget(), evt.generation);
  }
  return false;
}

@wrapMethod(VehicleObject)
protected cb func OnVehicleBumpEvent(evt: ref<VehicleBumpEvent>) -> Bool {
  let result: Bool = wrappedMethod(evt);
  if !IsDefined(evt) || !ChromeAxis_FeedbackEnabled() {
    return result;
  }
  let velocityChange: Float = AbsF(evt.impactVelocityChange);
  if !(velocityChange > 0.0) {
    return result;
  }

  let vehicle: ref<VehicleObject>;
  let normalSign: Float = 1.0;
  if this.IsPlayerDriver() {
    vehicle = this;
  } else {
    // Traffic can receive the event when the player's vehicle is the one that hit it.
    let other: ref<VehicleObject> = evt.hitVehicle;
    if !IsDefined(other) || !other.IsPlayerDriver() {
      return result;
    }
    vehicle = other;
    normalSign = -1.0;
  }

  let right: Vector4 = vehicle.GetWorldRight();
  let normal: Vector3 = evt.hitNormal;
  let lateral: Float = normalSign * (right.X * normal.X + right.Y * normal.Y + right.Z * normal.Z);
  ChromeAxis_VehicleImpact(vehicle, velocityChange, ClampF(lateral, -1.0, 1.0));
  return result;
}

@wrapMethod(VehicleComponent)
protected cb func OnUnmountingEvent(evt: ref<UnmountingEvent>) -> Bool {
  let vehicle: ref<VehicleObject> = this.GetVehicle();
  if IsDefined(vehicle) && IsDefined(evt) && IsDefined(evt.request) {
    let game: GameInstance = vehicle.GetGame();
    let actor: ref<GameObject> = GameInstance.FindEntityByID(game, evt.request.lowLevelMountingInfo.childId) as GameObject;
    if IsDefined(actor) && actor.IsPlayer() {
      ChromeAxis_DriverHeartbeat(null, false, new Vector4(0.0, 0.0, 0.0, 0.0));
    }
  }
  return wrappedMethod(evt);
}

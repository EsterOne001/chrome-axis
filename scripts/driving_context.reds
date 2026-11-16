@if(ModuleExists("HudPainter"))
import HudPainter.{ComponentColorItem, HudPainterStorage}
@if(ModuleExists("HudPainter"))
import Codeware.UI.TextInput
@if(ModuleExists("HudPainter"))
import RedData.Json.{JsonArray, JsonObject, JsonVariant}
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
class ChromeAxisHudPainterMenu extends ScriptableService {
  private let storage: ref<FileSystemStorage>;
  private let colors: ref<JsonObject>;
  private let palette: ref<ChromeAxisHudPalette>;
  private let activePresetKey: String;
  private let dirty: Bool;
  private let flushScheduled: Bool;
  private let pendingPresetKey: String;
  private let pendingPresetPalette: ref<ChromeAxisHudPalette>;
  private let pendingPresetAlreadyExisted: Bool;
  private let menuController: wref<inkGameController>;

  public static func Get() -> ref<ChromeAxisHudPainterMenu> {
    return GameInstance.GetScriptableServiceContainer().GetService(n"ChromeAxisHudPainterMenu") as ChromeAxisHudPainterMenu;
  }

  private cb func OnLoad() {
    // RedFileSystem grants this storage handle once per game launch.
    this.storage = FileSystem.GetStorage("ChromeAxisHUD");
    this.colors = this.ReadColors();
    this.palette = this.ReadPalette(this.colors, ChromeAxisHudPalette.Default());
    GameInstance.GetCallbackSystem()
      .RegisterCallback(n"InkWidget/Spawn", this, n"OnHudPainterWidgetSpawn")
      .AddTarget(inkWidgetTarget.Library(r"base\\gameplay\\gui\\hud_painter.inkwidget", n"ColorPicker"))
      .AddTarget(inkWidgetTarget.Controller(n"HudPainter.CustomColorPickerController"));
  }

  public func GetPalette() -> ref<ChromeAxisHudPalette> {
    if Equals(this.activePresetKey, "") {
      this.SyncActivePreset();
    }
    return this.palette;
  }

  public func SyncActivePreset() -> Bool {
    let painter = HudPainterStorage.Get();
    if !IsDefined(painter) {
      return false;
    }
    let name = painter.GetActivePresetName();
    let key = StrLower(name);
    if Equals(key, "") || Equals(key, this.activePresetKey) {
      return false;
    }

    this.FlushColors();
    this.activePresetKey = key;
    // Pre-0.4.50 palettes were global. Use them as the starting colors for profiles without overrides.
    let fallback = this.ReadPalette(this.colors, ChromeAxisHudPalette.Default());
    let presets = this.colors.GetKey("presets") as JsonObject;
    this.palette = IsDefined(presets) ? this.ReadPalette(presets.GetKey(key) as JsonObject, fallback) : fallback;
    return true;
  }

  public func ChangeColor(isBorder: Bool, color: HDRColor) -> Void {
    let current = this.GetPalette();
    let previous = isBorder ? current.border : current.transmission;
    if Equals(previous.Red, color.Red) && Equals(previous.Green, color.Green) && Equals(previous.Blue, color.Blue) {
      return;
    }
    let next = new ChromeAxisHudPalette();
    next.border = isBorder ? color : current.border;
    next.transmission = isBorder ? current.transmission : color;
    this.palette = next;
    this.StorePalette(this.activePresetKey, next);
    this.ApplyToHud();

    // Batch slider changes. Release and menu close also flush while the game is paused.
    if !this.flushScheduled {
      let delay = GameInstance.GetDelaySystem(GetGameInstance());
      if IsDefined(delay) {
        let callback = new ChromeAxisHudColorFlush();
        callback.menu = this;
        this.flushScheduled = true;
        delay.DelayCallback(callback, 0.30, false);
      }
    }
  }

  public func FlushAfterInput() -> Void {
    this.flushScheduled = false;
    this.FlushColors();
  }

  public func TrackPresetSave(name: String) -> Void {
    this.pendingPresetKey = StrLower(name);
    this.pendingPresetPalette = this.GetPalette();
    this.pendingPresetAlreadyExisted = this.HasNativePreset(this.pendingPresetKey);
  }

  public func FinishPresetSave() -> Void {
    if !IsDefined(this.pendingPresetPalette) || this.pendingPresetAlreadyExisted {
      return;
    }
    // Existence proves success only when the new name did not exist before the save.
    if this.HasNativePreset(this.pendingPresetKey) {
      this.PresetSaved();
    } else {
      this.pendingPresetPalette = null;
      this.pendingPresetKey = "";
    }
  }

  private func HasNativePreset(key: String) -> Bool {
    let painter = HudPainterStorage.Get();
    if IsDefined(painter) {
      for preset in painter.GetAvailablePresetsList() {
        let name = preset.name;
        if Equals(StrLower(name), key) {
          return true;
        }
      }
    }
    return false;
  }

  public func PresetSaved() -> Void {
    if IsDefined(this.pendingPresetPalette) {
      this.StorePalette(this.pendingPresetKey, this.pendingPresetPalette);
      this.pendingPresetPalette = null;
      this.pendingPresetKey = "";
    }
    this.FlushColors();
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

  private func StorePalette(key: String, palette: ref<ChromeAxisHudPalette>) -> Void {
    if Equals(key, "") {
      return;
    }
    let presets = this.colors.GetKey("presets") as JsonObject;
    if !IsDefined(presets) {
      presets = new JsonObject();
    }
    let entry = new JsonObject();
    entry.SetKey("border", this.EncodeColor(palette.border));
    entry.SetKey("transmission", this.EncodeColor(palette.transmission));
    presets.SetKey(key, entry);
    this.colors.SetKey("presets", presets);
    this.dirty = true;
  }

  public func FlushColors() -> Bool {
    if !this.dirty {
      return true;
    }
    if !IsDefined(this.storage) {
      return false;
    }
    let file = this.storage.GetFile("colors.json");
    if !IsDefined(file) || !file.WriteJson(this.colors) {
      return false;
    }
    this.dirty = false;
    return true;
  }

  private func ReadColors() -> ref<JsonObject> {
    if IsDefined(this.storage) && Equals(this.storage.IsFile("colors.json"), FileSystemStatus.True) {
      let file = this.storage.GetFile("colors.json");
      if IsDefined(file) {
        let colors = file.ReadAsJson() as JsonObject;
        if IsDefined(colors) {
          return colors;
        }
      }
    }
    return new JsonObject();
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
    this.menuController = controller;
    let root = controller.GetRootCompoundWidget();
    if IsDefined(root) && !IsDefined(root.GetWidgetByPathName(n"ChromeAxisPresetEvents")) {
      let events = new ChromeAxisHudPainterPresetEvents();
      events.Reparent(root);
    }
    this.RefreshMenu();
  }

  private cb func OnHudPainterWidgetSpawn(event: ref<inkWidgetSpawnEvent>) {
    // HUD Painter recreates the picker after rebuilding its color list.
    this.RefreshMenu();
  }

  private func RefreshMenu() -> Void {
    if this.SyncActivePreset() {
      this.ApplyToHud();
    }
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
      this.menu.FlushAfterInput();
    }
  }
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
      menu.PresetSaved();
    }
    return false;
  }

  protected cb func OnHudPainterInkStyleRefreshed(event: ref<HudPainterInkStyleRefreshed>) -> Bool {
    let menu = ChromeAxisHudPainterMenu.Get();
    if IsDefined(menu) && menu.SyncActivePreset() {
      menu.ApplyToHud();
    }
    return false;
  }
}

@if(ModuleExists("HudPainter"))
@wrapMethod(GenericMessageNotification)
private final func Close(result: GenericMessageNotificationResult) -> Void {
  let saveMenu: ref<ChromeAxisHudPainterMenu>;
  if Equals(result, GenericMessageNotificationResult.Confirm)
    && IsDefined(this.m_data) && this.m_data.isInput && IsDefined(this.m_textInput)
    && Equals(this.m_data.title, GetLocalizedTextByKey(n"Mod-HudPainter-Save-Preset-Title")) {
    let name = this.m_textInput.GetText();
    if NotEquals(name, "") && NotEquals(StrLower(name), "default") {
      saveMenu = ChromeAxisHudPainterMenu.Get();
      if IsDefined(saveMenu) {
        saveMenu.TrackPresetSave(name);
      }
    }
  }
  wrappedMethod(result);
  if IsDefined(saveMenu) {
    saveMenu.FinishPresetSave();
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

    this.message = this.AddLabel(panel, this.french ? "Enregistrement automatique" : "Autosave enabled", 24);
    this.message.SetMargin(32.0, 2.0, 0.0, 0.0);
  }

  protected cb func OnUninitialize() {
    let menu = ChromeAxisHudPainterMenu.Get();
    if IsDefined(menu) {
      menu.FlushColors();
    }
  }

  protected cb func OnHudPainterColorSelected(event: ref<HudPainterColorSelected>) -> Bool {
    this.selectedColor = event.data;
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
    if !IsDefined(this.border) || !IsDefined(this.transmission) || !IsDefined(this.message) || !IsDefined(event.color) {
      return false;
    }
    let isBorder = Equals(event.color, this.border.GetData());
    if isBorder || Equals(event.color, this.transmission.GetData()) {
      let menu = ChromeAxisHudPainterMenu.Get();
      if IsDefined(menu) {
        menu.ChangeColor(isBorder, event.color.customColor);
        if !menu.FlushColors() {
          this.message.SetText(this.french ? "Enregistrement impossible" : "Could not save colors");
        } else {
          this.border.GetData().presetColor = this.border.GetData().customColor;
          this.transmission.GetData().presetColor = this.transmission.GetData().customColor;
          this.border.RefreshItemData();
          this.transmission.RefreshItemData();
          this.message.SetText(this.french ? "Enregistrement automatique" : "Autosave enabled");
        }
      }
    }
    return false;
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

public static native func ChromeAxis_DriverHeartbeat(vehicle: ref<VehicleObject>) -> Int32;
public static native func ChromeAxis_CaptureEnabled() -> Bool;
public static native func ChromeAxis_CaptureVehicleRecord(vehicle: ref<VehicleObject>, record: TweakDBID) -> Void;
public static native func ChromeAxis_TransmissionState(vehicle: ref<VehicleObject>) -> Int32;
public static native func ChromeAxis_FeedbackEnabled() -> Bool;
public static native func ChromeAxis_FeedbackMotion(vehicle: ref<VehicleObject>, velocity: Vector4, forward: Vector4, right: Vector4, mass: Float, simTime: Float, skidding: Bool, airborne: Bool) -> Void;
public static native func ChromeAxis_VehicleImpact(vehicle: ref<VehicleObject>, velocityChange: Float, lateral: Float) -> Void;

public class ChromeAxisDrivingContext extends ScriptableSystem {
  private let player: wref<PlayerPuppet>;
  private let generation: Int32;
  private let feedbackVehicle: wref<VehicleObject>;
  private let feedbackPolling: Bool;
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
    this.player = request.owner as PlayerPuppet;
    this.generation += 1;
    this.feedbackVehicle = null;
    this.feedbackPolling = false;
    ChromeAxis_DriverHeartbeat(null);
    this.Refresh(this.generation);
  }

  private func OnPlayerDetach(request: ref<PlayerDetachRequest>) -> Void {
    this.Clear();
  }

  public func OnDetach() -> Void {
    this.Clear();
  }

  private func Clear() -> Void {
    this.player = null;
    this.generation += 1;
    this.feedbackVehicle = null;
    this.feedbackPolling = false;
    ChromeAxis_DriverHeartbeat(null);
    if IsDefined(this.hudBadge) {
      let parent = this.hudBadge.GetParentWidget() as inkCompoundWidget;
      if IsDefined(parent) {
        parent.RemoveChild(this.hudBadge);
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

    if IsDefined(this.hudBadge) {
      if NotEquals(this.hudBadge.GetParentWidget(), rootWidget) {
        this.hudBadge.Reparent(root);
        this.ApplyHudPalette();
        this.ApplyHudStatus();
      }
      return true;
    }

    let badge = new inkCanvas();
    badge.SetName(n"ChromeAxisStatusBadge");
    badge.SetSize(Vector2(236.0, 48.0));
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
    logo.SetTranslation(16.0, 12.0);
    logo.Reparent(badge);

    let shadow = this.CreateHudLabel(badge, 43.0, 5.0, 130.0, 21.0, 17, textHorizontalAlignment.Left);
    shadow.SetTintColor(HDRColor(0.0, 0.0, 0.0, 0.85));
    let label = this.CreateHudLabel(badge, 42.0, 4.0, 130.0, 21.0, 17, textHorizontalAlignment.Left);

    let modeLabel = this.CreateHudLabel(badge, 42.0, 24.0, 130.0, 17.0, 13, textHorizontalAlignment.Left);
    let gearCaption = this.CreateHudLabel(badge, 180.0, 5.0, 44.0, 14.0, 10, textHorizontalAlignment.Center);
    let gearLabel = this.CreateHudLabel(badge, 180.0, 14.0, 44.0, 29.0, 27, textHorizontalAlignment.Center);

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
    let panel = HDRColor(0.024, 0.039, 0.059, 0.72);
    this.CreateHudRect(badge, 0.0, 2.0, 227.0, 44.0).SetTintColor(panel);
    this.CreateHudRect(badge, 227.0, 2.0, 9.0, 36.0).SetTintColor(panel);
    this.hudInnerFrame = this.CreateHudDetail(badge, Vector2(5.0, 7.0), Vector2(217.0, 34.0), 0.44, true);
    this.hudLogoFrame = this.CreateHudDetail(badge, Vector2(15.0, 11.0), Vector2(22.0, 22.0), 0.38, false);
    ArrayClear(this.hudBorder);
    ArrayPush(this.hudBorder, this.CreateHudRect(badge, 0.0, 2.0, 236.0, 1.0));
    ArrayPush(this.hudBorder, this.CreateHudRect(badge, 0.0, 3.0, 1.0, 42.0));
    ArrayPush(this.hudBorder, this.CreateHudRect(badge, 0.0, 45.0, 228.0, 1.0));
    ArrayPush(this.hudBorder, this.CreateHudRect(badge, 235.0, 3.0, 1.0, 34.0));
    ArrayPush(this.hudBorder, this.CreateHudRect(badge, 227.0, 37.0, 9.0, 1.0));
    ArrayPush(this.hudBorder, this.CreateHudRect(badge, 227.0, 38.0, 1.0, 7.0));
    ArrayPush(this.hudBorder, this.CreateHudRect(badge, 12.0, 0.0, 16.0, 2.0));
    this.hudDivider = this.CreateHudRect(badge, 173.0, 10.0, 1.0, 28.0);
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

  private func CanDrive(game: GameInstance) -> Bool {
    let stateBoard: ref<IBlackboard> = this.player.GetPlayerStateMachineBlackboard();
    if !IsDefined(stateBoard) {
      return false;
    }
    let state: gamePSMVehicle = IntEnum<gamePSMVehicle>(stateBoard.GetInt(GetAllBlackboardDefs().PlayerStateMachine.Vehicle));
    if NotEquals(state, gamePSMVehicle.Driving) && NotEquals(state, gamePSMVehicle.DriverCombat) {
      return false;
    }
    let boards: ref<BlackboardSystem> = GameInstance.GetBlackboardSystem(game);
    if !IsDefined(boards) {
      return false;
    }
    let ui: ref<IBlackboard> = boards.Get(GetAllBlackboardDefs().UI_System);
    let photoMode: ref<IBlackboard> = boards.Get(GetAllBlackboardDefs().PhotoMode);
    return IsDefined(ui) && !ui.GetBool(GetAllBlackboardDefs().UI_System.IsInMenu) &&
           (!IsDefined(photoMode) || !photoMode.GetBool(GetAllBlackboardDefs().PhotoMode.IsActive));
  }

  public func Refresh(generation: Int32) -> Void {
    if generation != this.generation || !IsDefined(this.player) {
      return;
    }
    let game: GameInstance = this.player.GetGame();
    let vehicle: wref<VehicleObject>;
    if !(this.CanDrive(game) && VehicleComponent.GetVehicle(game, this.player.GetEntityID(), vehicle) &&
         IsDefined(vehicle) && VehicleComponent.IsDriver(game, this.player)) {
      vehicle = null;
    }
    let status = ChromeAxis_DriverHeartbeat(vehicle);
    if IsDefined(vehicle) && ChromeAxis_CaptureEnabled() {
      ChromeAxis_CaptureVehicleRecord(vehicle, vehicle.GetRecordID());
    }
    this.feedbackVehicle = vehicle;
    if IsDefined(vehicle) && !this.feedbackPolling && ChromeAxis_FeedbackEnabled() {
      this.feedbackPolling = true;
      this.RefreshFeedback(generation);
    }

    let next: ref<ChromeAxisDrivingTick> = new ChromeAxisDrivingTick();
    next.context = this;
    next.generation = generation;
    GameInstance.GetDelaySystem(game).DelayCallback(next, 0.10, false);
    this.UpdateHud(game, status, vehicle);
  }

  public func RefreshFeedback(generation: Int32) -> Void {
    if generation != this.generation || !IsDefined(this.player) {
      return;
    }
    if !ChromeAxis_FeedbackEnabled() {
      this.feedbackPolling = false;
      return;
    }
    let game: GameInstance = this.player.GetGame();
    let vehicle = this.feedbackVehicle;
    if !IsDefined(vehicle) || !this.CanDrive(game) || !vehicle.IsPlayerDriver() {
      this.feedbackPolling = false;
      ChromeAxis_DriverHeartbeat(null);
      return;
    }

    let airborne: Bool = vehicle.IsInAir();
    ChromeAxis_FeedbackMotion(vehicle, vehicle.GetLinearVelocity(), vehicle.GetWorldForward(), vehicle.GetWorldRight(),
      vehicle.GetTotalMass(), EngineTime.ToFloat(GameInstance.GetSimTime(game)), !airborne && vehicle.IsSkidding(2.5), airborne);

    let next: ref<ChromeAxisFeedbackTick> = new ChromeAxisFeedbackTick();
    next.context = this;
    next.generation = generation;
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

  public func Call() -> Void {
    if IsDefined(this.context) {
      this.context.RefreshFeedback(this.generation);
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
  if this.IsA(n"gameuiRootHudGameController") && !evt.attached && IsDefined(evt.context) {
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
      ChromeAxis_DriverHeartbeat(null);
    }
  }
  return wrappedMethod(evt);
}

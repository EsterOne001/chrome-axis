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
  private let hudLogo: wref<inkImage>;
  private let hudLogoReady: Bool;
  private let hudLogoRetry: Int32;
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
    this.hudLogo = null;
    this.hudLogoReady = false;
    this.hudLogoRetry = 0;
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
    logo.SetSize(Vector2(26.0, 26.0));
    logo.SetTranslation(9.0, 10.0);
    logo.Reparent(badge);

    let shadow = this.CreateHudLabel(badge, 43.0, 5.0, 130.0, 21.0, 17, textHorizontalAlignment.Left);
    shadow.SetTintColor(HDRColor(0.0, 0.0, 0.0, 0.85));
    let label = this.CreateHudLabel(badge, 42.0, 4.0, 130.0, 21.0, 17, textHorizontalAlignment.Left);

    let secondary = HDRColor(0.65, 0.78, 0.82, 1.0);
    let modeLabel = this.CreateHudLabel(badge, 42.0, 24.0, 130.0, 17.0, 13, textHorizontalAlignment.Left);
    modeLabel.SetTintColor(secondary);
    let gearCaption = this.CreateHudLabel(badge, 180.0, 5.0, 44.0, 14.0, 10, textHorizontalAlignment.Center);
    gearCaption.SetTintColor(secondary);
    let gearLabel = this.CreateHudLabel(badge, 180.0, 16.0, 44.0, 29.0, 27, textHorizontalAlignment.Center);
    gearLabel.SetTintColor(HDRColor(0.87, 0.96, 1.0, 1.0));

    this.hudBadge = badge;
    this.hudLogo = logo;
    this.hudLogoRetry = 30;
    this.hudLabel = label;
    this.hudLabelShadow = shadow;
    this.hudGearLabel = gearLabel;
    this.hudGearCaption = gearCaption;
    this.hudModeLabel = modeLabel;
    this.ApplyHudStatus();
    this.ApplyHudGear();
    this.TryShowHudLogo();
    return true;
  }

  private func TryShowHudLogo() -> Void {
    if this.hudLogoReady || this.hudLogoRetry <= 0 || !IsDefined(this.hudLogo) {
      return;
    }
    this.hudLogoRetry -= 1;
    // Ink can show a solid placeholder until the atlas part is available.
    if this.hudLogo.IsTexturePartExist(n"logo") && this.hudLogo.SetTexturePart(n"logo") {
      this.hudLogoReady = true;
      this.hudLogo.SetVisible(true);
    }
  }

  private func BuildHudFrame(badge: ref<inkCanvas>) -> Void {
    let panel = HDRColor(0.024, 0.039, 0.059, 0.72);
    this.AddHudRect(badge, 0.0, 2.0, 227.0, 44.0, panel);
    this.AddHudRect(badge, 227.0, 2.0, 9.0, 36.0, panel);
    let border = HDRColor(1.0, 0.32, 0.40, 0.86);
    this.AddHudRect(badge, 0.0, 2.0, 236.0, 1.0, border);
    this.AddHudRect(badge, 0.0, 3.0, 1.0, 42.0, border);
    this.AddHudRect(badge, 0.0, 45.0, 228.0, 1.0, border);
    this.AddHudRect(badge, 235.0, 3.0, 1.0, 34.0, border);
    this.AddHudRect(badge, 227.0, 37.0, 9.0, 1.0, border);
    this.AddHudRect(badge, 227.0, 38.0, 1.0, 7.0, border);
    this.AddHudRect(badge, 12.0, 0.0, 16.0, 2.0, border);
    this.AddHudRect(badge, 173.0, 8.0, 1.0, 31.0, HDRColor(0.42, 0.65, 0.69, 0.45));
  }

  private func AddHudRect(badge: ref<inkCanvas>, x: Float, y: Float, width: Float, height: Float, color: HDRColor) -> Void {
    let rectangle = new inkRectangle();
    rectangle.SetInteractive(false);
    rectangle.SetSize(Vector2(width, height));
    rectangle.SetTranslation(x, y);
    rectangle.SetTintColor(color);
    rectangle.Reparent(badge);
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
      this.hudFrench = this.IsFrenchInterface(game);
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
        if !this.hudLogoReady {
          this.hudLogoRetry = 30;
        }
      }
      if changed || gearLineChanged {
        this.ApplyHudGear();
      }
      this.TryShowHudLogo();
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

  private func IsFrenchInterface(game: GameInstance) -> Bool {
    let settings: ref<UserSettings> = GameInstance.GetSettingsSystem(game);
    if !IsDefined(settings) {
      return false;
    }
    let language: ref<ConfigVarListName> = settings.GetVar(n"/language", n"OnScreen") as ConfigVarListName;
    return IsDefined(language) && Equals(language.GetValue(), n"fr-fr");
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

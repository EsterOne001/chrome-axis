public static native func ChromeAxis_DriverHeartbeat(vehicle: ref<VehicleObject>) -> Int32;
public static native func ChromeAxis_FeedbackEnabled() -> Bool;
public static native func ChromeAxis_VehicleMotion(vehicle: ref<VehicleObject>, forwardSpeed: Float, sideSpeed: Float, mass: Float) -> Void;
public static native func ChromeAxis_VehicleImpact(vehicle: ref<VehicleObject>, velocityChange: Float, lateral: Float) -> Void;

public class ChromeAxisDrivingContext extends ScriptableSystem {
  private let player: wref<PlayerPuppet>;
  private let generation: Int32;
  private let hudRoot: wref<inkCompoundWidget>;
  private let hudBadge: wref<inkCanvas>;
  private let hudLogo: wref<inkImage>;
  private let hudLogoReady: Bool;
  private let hudLogoRetry: Int32;
  private let hudLabel: wref<inkText>;
  private let hudLabelShadow: wref<inkText>;
  private let hudStatus: Int32;
  private let hudRetry: Int32;
  private let hudFrench: Bool;

  private func OnPlayerAttach(request: ref<PlayerAttachRequest>) -> Void {
    this.player = request.owner as PlayerPuppet;
    this.generation += 1;
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
    ChromeAxis_DriverHeartbeat(null);
    if IsDefined(this.hudBadge) && IsDefined(this.hudRoot) {
      this.hudRoot.RemoveChild(this.hudBadge);
    }
    this.hudRoot = null;
    this.hudBadge = null;
    this.hudLogo = null;
    this.hudLogoReady = false;
    this.hudLogoRetry = 0;
    this.hudLabel = null;
    this.hudLabelShadow = null;
    this.hudStatus = 0;
    this.hudRetry = 0;
    this.hudFrench = false;
  }

  public func AttachHud(root: ref<inkCompoundWidget>) -> Void {
    if !IsDefined(root) || IsDefined(this.hudBadge) || this.hudStatus == 0 {
      return;
    }

    let badge = new inkCanvas();
    badge.SetName(n"ChromeAxisStatusBadge");
    badge.SetSize(Vector2(176.0, 34.0));
    badge.SetAnchor(inkEAnchor.TopLeft);
    badge.SetAnchorPoint(0.0, 0.0);
    badge.SetTranslation(36.0, 176.0);
    badge.SetInteractive(false);
    badge.Reparent(root);

    let logo = new inkImage();
    logo.SetVisible(false);
    logo.SetAtlasResource(r"chrome_axis\\hud\\logo.inkatlas");
    logo.SetSize(Vector2(34.0, 34.0));
    logo.Reparent(badge);

    let shadow = this.CreateHudLabel(badge, 1.0);
    shadow.SetTintColor(HDRColor(0.0, 0.0, 0.0, 0.85));
    let label = this.CreateHudLabel(badge, 0.0);

    this.hudRoot = root;
    this.hudBadge = badge;
    this.hudLogo = logo;
    this.hudLogoRetry = 30;
    this.hudLabel = label;
    this.hudLabelShadow = shadow;
    this.ApplyHudStatus();
    this.TryShowHudLogo();
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

  private func CreateHudLabel(badge: ref<inkCanvas>, offset: Float) -> ref<inkText> {
    let label = new inkText();
    label.SetFontFamily("base\\gameplay\\gui\\fonts\\raj\\raj.inkfontfamily");
    label.SetFontStyle(n"Bold");
    label.SetFontSize(18);
    label.SetHorizontalAlignment(textHorizontalAlignment.Left);
    label.SetVerticalAlignment(textVerticalAlignment.Center);
    label.SetSize(Vector2(134.0, 34.0));
    label.SetTranslation(40.0 + offset, offset);
    label.Reparent(badge);
    return label;
  }

  private func UpdateHud(game: GameInstance, status: Int32) -> Void {
    if status != 0 && this.hudStatus == 0 {
      this.hudFrench = this.IsFrenchInterface(game);
    }
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
      this.TryShowHudLogo();
      return;
    }
    if this.hudRetry > 0 {
      this.hudRetry -= 1;
      return;
    }
    // The HUD may still be initializing when loading into a vehicle.
    this.hudRetry = 9;
    let ui = GameInstance.GetUISystem(game);
    if IsDefined(ui) {
      let event = new ChromeAxisAttachHudEvent();
      event.context = this;
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

  public func Refresh(generation: Int32) -> Void {
    if generation != this.generation || !IsDefined(this.player) {
      return;
    }

    let game: GameInstance = this.player.GetGame();
    let vehicle: wref<VehicleObject>;
    let stateBoard: ref<IBlackboard> = this.player.GetPlayerStateMachineBlackboard();
    let canDrive: Bool = false;

    if IsDefined(stateBoard) {
      let state: gamePSMVehicle = IntEnum<gamePSMVehicle>(stateBoard.GetInt(GetAllBlackboardDefs().PlayerStateMachine.Vehicle));
      if Equals(state, gamePSMVehicle.Driving) || Equals(state, gamePSMVehicle.DriverCombat) {
        let boards: ref<BlackboardSystem> = GameInstance.GetBlackboardSystem(game);
        if IsDefined(boards) {
          let ui: ref<IBlackboard> = boards.Get(GetAllBlackboardDefs().UI_System);
          let photoMode: ref<IBlackboard> = boards.Get(GetAllBlackboardDefs().PhotoMode);
          canDrive = IsDefined(ui) && !ui.GetBool(GetAllBlackboardDefs().UI_System.IsInMenu) &&
                     (!IsDefined(photoMode) || !photoMode.GetBool(GetAllBlackboardDefs().PhotoMode.IsActive)) &&
                     VehicleComponent.GetVehicle(game, this.player.GetEntityID(), vehicle) &&
                     IsDefined(vehicle) && VehicleComponent.IsDriver(game, this.player);
        }
      }
    }

    if !canDrive {
      vehicle = null;
    }
    this.UpdateHud(game, ChromeAxis_DriverHeartbeat(vehicle));
    if canDrive && ChromeAxis_FeedbackEnabled() {
      let velocity: Vector4 = vehicle.GetLinearVelocity();
      let forwardSpeed: Float = Vector4.Dot(velocity, vehicle.GetWorldForward());
      let sideSpeed: Float = Vector4.Dot(velocity, vehicle.GetWorldRight());
      ChromeAxis_VehicleMotion(vehicle, forwardSpeed, sideSpeed, vehicle.GetTotalMass());
    }

    let next: ref<ChromeAxisDrivingTick> = new ChromeAxisDrivingTick();
    next.context = this;
    next.generation = generation;
    GameInstance.GetDelaySystem(game).DelayCallback(next, 0.10, false);
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

public class ChromeAxisAttachHudEvent extends Event {
  public let context: wref<ChromeAxisDrivingContext>;
}

@addMethod(inkGameController)
protected cb func OnChromeAxisAttachHudEvent(evt: ref<ChromeAxisAttachHudEvent>) -> Bool {
  if this.IsA(n"gameuiRootHudGameController") && IsDefined(evt.context) {
    evt.context.AttachHud(this.GetRootCompoundWidget());
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

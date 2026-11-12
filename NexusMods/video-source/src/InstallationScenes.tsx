import {useCurrentFrame, useVideoConfig} from 'remotion';
import {Accent, capture, Columns, Evidence, heading, MainCopy, Note, palette, PathLabel, Scene, type Locale} from './Visuals';

export const Archive = ({locale}: {locale: Locale}) => {
  const fr = locale === 'fr';
  const shot = capture('01-archive.png');

  return (
    <Scene index="03A" title={fr ? 'Extrayez les deux dossiers' : 'Extract both folders'} label={fr ? 'Installation manuelle' : 'Manual installation'}>
      <div style={{height: '100%', display: 'flex', flexDirection: 'column', alignItems: 'center', justifyContent: 'center', gap: 30}}>
        <MainCopy>{fr ? <>Ouvrez le ZIP et extrayez <Accent>archive</Accent> et <Accent>red4ext</Accent>.</> : <>Open the ZIP and extract <Accent>archive</Accent> and <Accent>red4ext</Accent>.</>}</MainCopy>
        {shot ? <Evidence src={shot} maxWidth={1160} maxHeight={305} focus={[.1, 15.8, 29.7, 27.2]} caption={fr ? 'Archive officielle Chrome Axis 0.4.15 ouverte dans NanaZip' : 'Official Chrome Axis 0.4.15 archive opened in NanaZip'} /> : <PathLabel>archive &nbsp;&nbsp; red4ext</PathLabel>}
        <Note>{fr ? 'Choisissez comme destination le dossier qui contient bin.' : 'Choose the folder that contains bin as the extraction destination.'}</Note>
      </div>
    </Scene>
  );
};

export const GameFolder = ({locale}: {locale: Locale}) => {
  const fr = locale === 'fr';
  const shot = capture('02-game-folder.png');

  return (
    <Scene index="03A" title={fr ? 'Visez le dossier du jeu' : 'Use the game’s root folder'} label={fr ? 'Installation manuelle' : 'Manual installation'}>
      <div style={{height: '100%', display: 'flex', flexDirection: 'column', alignItems: 'center', justifyContent: 'center', gap: 24}}>
        <MainCopy>{fr ? <><Accent>archive</Accent> et <Accent>red4ext</Accent> doivent se trouver à côté de <Accent>bin</Accent>.</> : <><Accent>archive</Accent> and <Accent>red4ext</Accent> should sit alongside <Accent>bin</Accent>.</>}</MainCopy>
        {shot ? <Evidence src={shot} maxWidth={1120} maxHeight={356} zoom={1.03} focus={[22.6, 64.8, 31, 30.5]} caption={fr ? 'Exemple dans un dossier de démonstration Windows' : 'Example in a Windows demonstration folder'} /> : <PathLabel>Cyberpunk 2077 &nbsp; archive &nbsp; bin &nbsp; red4ext</PathLabel>}
        <div style={{display: 'grid', gap: 12, width: 1130}}>
          <Note>{fr ? 'Si ces dossiers existent déjà, laissez Windows les fusionner.' : 'If these folders already exist, let Windows merge them.'}</Note>
          <Note>{fr ? 'Steam : Bibliothèque → clic droit sur Cyberpunk 2077 → Gérer → Parcourir les fichiers locaux.' : 'Steam: Library → right-click Cyberpunk 2077 → Manage → Browse local files.'}</Note>
        </div>
      </div>
    </Scene>
  );
};

const VortexSteps = ({locale, deployed}: {locale: Locale; deployed: boolean}) => {
  const fr = locale === 'fr';
  const steps = deployed
    ? fr ? ['Activez Chrome Axis', 'Déployez les mods', 'Ouvrir → Open Game Folder'] : ['Enable Chrome Axis', 'Click Deploy Mods', 'Open → Open Game Folder']
    : fr ? ['Mods → Ajouter un fichier (+)', 'Sélectionnez le ZIP de Chrome Axis'] : ['Mods → Install From File (+)', 'Select the Chrome Axis ZIP'];

  return (
    <div style={{display: 'grid', gap: 17}}>
      {steps.map((label, index) => <div key={label} style={{display: 'flex', gap: 17, alignItems: 'center', fontSize: 34, lineHeight: 1.12, background: palette.panel, border: `1px solid ${palette.line}`, padding: '18px 21px'}}><span style={{fontFamily: heading, color: palette.yellow, fontSize: 38}}>{index + (deployed ? 3 : 1)}</span>{label}</div>)}
    </div>
  );
};

export const Vortex = ({locale}: {locale: Locale}) => {
  const fr = locale === 'fr';
  const shot = capture('04-vortex.png');
  const toolbar = capture('04-vortex-toolbar.png');
  const openGameFolder = capture('08-vortex-game-folder.png');
  const frame = useCurrentFrame();
  const {fps} = useVideoConfig();
  const deployed = frame >= 4.5 * fps;

  return (
    <Scene index="03B" title={fr ? 'Ou installez via Vortex' : 'Or install through Vortex'} label={fr ? 'Autre méthode' : 'Alternative method'}>
      <Columns>
        <div><VortexSteps locale={locale} deployed={deployed} /></div>
        {deployed ? (openGameFolder ? <Evidence key="open" src={openGameFolder} caption={fr ? 'Vortex · commande du menu Ouvrir' : 'Vortex · Open menu command'} maxHeight={252} maxWidth={520} focus={[1.5, 34, 95, 29]} /> : <Note>{fr ? 'Le menu Ouvrir de Vortex permet de retrouver la racine de Cyberpunk 2077.' : 'The Vortex Open menu takes you to the Cyberpunk 2077 root.'}</Note>) : (
          <div style={{display: 'grid', gap: 14}}>
            {toolbar ? <Evidence key="toolbar" src={toolbar} caption={fr ? 'Vortex · Ajouter un fichier (+)' : 'Vortex · Install From File (+)'} maxWidth={870} maxHeight={62} focus={[73.8, 27, 3.6, 47]} /> : null}
            {shot ? <Evidence key="picker" src={shot} caption={fr ? 'Fenêtre réelle de sélection du ZIP' : 'Actual ZIP selection window'} maxHeight={422} maxWidth={870} focus={[0.8, 5.2, 59.5, 4.8]} /> : null}
          </div>
        )}
      </Columns>
    </Scene>
  );
};

export const Executable = ({locale}: {locale: Locale}) => {
  const fr = locale === 'fr';
  const shot = capture('03-executable.png');
  const frame = useCurrentFrame();
  const {fps} = useVideoConfig();
  const showFile = frame >= 4.8 * fps;

  return (
    <Scene index="04" title={fr ? 'Ouvrez ChromeAxis.exe' : 'Open ChromeAxis.exe'} label={fr ? 'Application de réglage' : 'Setup utility'}>
      <div style={{height: '100%', display: 'flex', flexDirection: 'column', alignItems: 'center', justifyContent: 'center', gap: 20}}>
        <PathLabel>red4ext\plugins\chrome_axis\ChromeAxis.exe</PathLabel>
        {shot ? <Evidence src={shot} maxWidth={1120} maxHeight={420} focus={showFile ? [22.7, 67.5, 67.8, 7.3] : [31.1, 13.1, 34.9, 7.7]} zoom={1.02} caption={fr ? 'Explorateur Windows · dossier de démonstration' : 'Windows File Explorer · demonstration folder'} /> : null}
        <Note>{fr ? 'Depuis la racine de Cyberpunk 2077, ouvrez ce dossier et double-cliquez sur ChromeAxis.exe.' : 'From the Cyberpunk 2077 root, open this folder and double-click ChromeAxis.exe.'}</Note>
      </div>
    </Scene>
  );
};

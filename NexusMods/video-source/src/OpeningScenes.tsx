import {AbsoluteFill, CanvasImage, Easing, interpolate, useCurrentFrame, useVideoConfig} from 'remotion';
import {Accent, asset, body, capture, Evidence, heading, Logo, MainCopy, Note, palette, Scene, type Locale} from './Visuals';

export const Intro = ({locale}: {locale: Locale}) => {
  const fr = locale === 'fr';
  const frame = useCurrentFrame();
  const {fps} = useVideoConfig();

  return (
    <AbsoluteFill style={{backgroundColor: palette.black, overflow: 'hidden', color: palette.text, fontFamily: body}}>
      <CanvasImage src={asset('wheel-art.png')} premountFor={fps} style={{position: 'absolute', inset: 0, width: '100%', height: '100%', objectFit: 'cover', scale: interpolate(frame, [0, 5 * fps], [1, 1.065], {extrapolateRight: 'clamp'}), transformOrigin: '76% 54%'}} />
      <AbsoluteFill style={{background: 'linear-gradient(90deg,#090b0ee8 2%,#090b0ebd 35%,#090b0e00 72%)'}} />
      <div style={{position: 'absolute', left: 110, top: 157, width: 734, translate: `0 ${interpolate(frame, [0, .42 * fps], [14, 0], {extrapolateRight: 'clamp', easing: Easing.out(Easing.cubic)})}px`}}>
        <div style={{height: 3, background: `linear-gradient(90deg,${palette.yellow} 84px,#586269 84px,transparent)`, marginBottom: 23}} />
        <div style={{fontFamily: heading, fontSize: 29, color: palette.cyan, letterSpacing: 3}}>CYBERPUNK 2077 / WINDOWS</div>
        <div style={{marginTop: 34, marginLeft: -16}}><Logo size={103} /></div>
        <div style={{color: palette.yellow, fontFamily: heading, fontSize: 129, lineHeight: 1.03, whiteSpace: 'nowrap', letterSpacing: 1}}>CHROME AXIS</div>
        <div style={{fontSize: 49, marginTop: 20, lineHeight: 1.08}}>{fr ? 'Installation et premier réglage' : 'Installation and first setup'}</div>
        <div style={{marginTop: 31, color: palette.muted, fontSize: 28, letterSpacing: .8}}>{fr ? 'Guide pour la version 0.4.15' : 'Guide for version 0.4.15'}</div>
      </div>
    </AbsoluteFill>
  );
};

export const Prerequisites = ({locale}: {locale: Locale}) => {
  const fr = locale === 'fr';

  return (
    <Scene index="01" title={fr ? 'Préparez le jeu' : 'Prepare the game'} label={fr ? 'Prérequis' : 'Requirements'}>
      <div style={{display: 'flex', flexDirection: 'column', justifyContent: 'center', height: '100%', gap: 29}}>
        <MainCopy>{fr ? 'Fermez Cyberpunk 2077. Installez les deux dépendances.' : 'Close Cyberpunk 2077. Install both dependencies.'}</MainCopy>
        <div style={{display: 'grid', gridTemplateColumns: '1fr 1fr', gap: 28}}>
          <div style={{padding: '28px 34px 33px', background: palette.panel, border: `1px solid ${palette.line}`, borderTop: `4px solid ${palette.cyan}`}}><div style={{fontSize: 58, fontFamily: heading, lineHeight: 1}}>RED4ext</div><div style={{fontSize: 37, color: palette.yellow, marginTop: 10}}>1.31.1</div></div>
          <div style={{padding: '28px 34px 33px', background: palette.panel, border: `1px solid ${palette.line}`, borderTop: `4px solid ${palette.cyan}`}}><div style={{fontSize: 58, fontFamily: heading, lineHeight: 1}}>redscript</div><div style={{fontSize: 37, color: palette.yellow, marginTop: 10}}>0.5.32</div></div>
        </div>
        <Note>{fr ? 'Cible de la version 0.4.15 : Cyberpunk 2077 2.5 sur Windows · exécutable 3.0.88.46235.' : 'Version 0.4.15 targets Cyberpunk 2077 2.5 on Windows · executable 3.0.88.46235.'}</Note>
      </div>
    </Scene>
  );
};

export const Download = ({locale}: {locale: Locale}) => {
  const fr = locale === 'fr';
  const files = capture('00-nexus-files.png');
  const download = capture('06-download.png');

  return (
    <Scene index="02" title={fr ? 'Téléchargez le ZIP' : 'Download the ZIP'} label="Nexus Mods">
      <div style={{display: 'flex', flexDirection: 'column', justifyContent: 'center', alignItems: 'center', height: '100%', gap: 26}}>
        <MainCopy>{fr ? <>Sur la page Chrome Axis, ouvrez <Accent>Files / Fichiers</Accent> et téléchargez le fichier principal.</> : <>On the Chrome Axis page, open <Accent>Files</Accent> and download the main file.</>}</MainCopy>
        <Evidence src={files ?? download ?? asset(`download-${locale}.png`)} maxWidth={1160} maxHeight={315} focus={files || !download ? undefined : [22.6, 79.6, 68.1, 12.4]} caption={files ? 'Nexus Mods · Chrome Axis' : download ? (fr ? 'Explorateur Windows · Téléchargements' : 'Windows File Explorer · Downloads') : (fr ? 'Visuel de la fiche Chrome Axis sur Nexus Mods' : 'Chrome Axis listing artwork for Nexus Mods')} />
        <div style={{fontFamily: 'Consolas, monospace', fontSize: 32, color: palette.cyan, padding: '15px 28px', background: '#0c1d23', border: '1px solid #2e6a71'}}>ChromeAxis-0.4.15-Cyberpunk-2.5.zip</div>
      </div>
    </Scene>
  );
};

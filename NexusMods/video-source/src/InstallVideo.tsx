import {useEffect, useState} from 'react';
import {Series, useDelayRender, useVideoConfig} from 'remotion';
import {Archive, Executable, GameFolder, Vortex} from './InstallationScenes';
import {Download, Intro, Prerequisites} from './OpeningScenes';
import {Hardware, Simulation} from './SetupScenes';
import {asset, type Locale} from './Visuals';

export const InstallVideo = ({locale}: {locale: Locale}) => {
  const {fps} = useVideoConfig();
  const {delayRender, continueRender, cancelRender} = useDelayRender();
  const [fontHandle] = useState(() => delayRender('Load the local Barlow fonts'));

  useEffect(() => {
    const condensed = new FontFace('Barlow Condensed', `url(${asset('BarlowCondensed-SemiBold.ttf')})`);
    const semiCondensed = new FontFace('Barlow Semi Condensed', `url(${asset('BarlowSemiCondensed-Medium.ttf')})`);
    Promise.all([condensed.load(), semiCondensed.load()]).then((fonts) => {
      fonts.forEach((font) => document.fonts.add(font));
      continueRender(fontHandle);
    }).catch((error: unknown) => cancelRender(error));
  }, [fontHandle, continueRender, cancelRender]);

  return (
    <Series>
      <Series.Sequence name="Opening" durationInFrames={5 * fps} premountFor={fps}><Intro locale={locale} /></Series.Sequence>
      <Series.Sequence name="Requirements" durationInFrames={7 * fps} premountFor={fps}><Prerequisites locale={locale} /></Series.Sequence>
      <Series.Sequence name="Nexus download" durationInFrames={8 * fps} premountFor={fps}><Download locale={locale} /></Series.Sequence>
      <Series.Sequence name="ZIP folders" durationInFrames={5 * fps} premountFor={fps}><Archive locale={locale} /></Series.Sequence>
      <Series.Sequence name="Game folder" durationInFrames={6 * fps} premountFor={fps}><GameFolder locale={locale} /></Series.Sequence>
      <Series.Sequence name="Vortex" durationInFrames={9 * fps} premountFor={fps}><Vortex locale={locale} /></Series.Sequence>
      <Series.Sequence name="Open ChromeAxis.exe" durationInFrames={14 * fps} premountFor={fps}><Executable locale={locale} /></Series.Sequence>
      <Series.Sequence name="Configure USB" durationInFrames={14 * fps} premountFor={fps}><Hardware locale={locale} /></Series.Sequence>
      <Series.Sequence name="Keyboard simulation" durationInFrames={8 * fps} premountFor={fps}><Simulation locale={locale} /></Series.Sequence>
    </Series>
  );
};

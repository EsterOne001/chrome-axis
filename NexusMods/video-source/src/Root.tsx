import {Composition} from 'remotion';
import {InstallVideo} from './InstallVideo';

export const Root = () => (
  <>
    <Composition id="ChromeAxisInstallationFR" component={InstallVideo} durationInFrames={2280} fps={30} width={1600} height={900} defaultProps={{locale: 'fr'}} />
    <Composition id="ChromeAxisInstallationEN" component={InstallVideo} durationInFrames={2280} fps={30} width={1600} height={900} defaultProps={{locale: 'en'}} />
  </>
);

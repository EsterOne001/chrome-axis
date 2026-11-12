import {useCurrentFrame, useVideoConfig} from 'remotion';
import {Accent, asset, capture, Columns, Evidence, MainCopy, Note, Scene, type Focus, type Locale} from './Visuals';

export const Hardware = ({locale}: {locale: Locale}) => {
  const fr = locale === 'fr';
  const frame = useCurrentFrame();
  const {fps} = useVideoConfig();
  const shot = capture(`05-chrome-axis-${locale}.png`) ?? asset(`hardware-${locale}.png`);
  const at = frame / fps;
  const focus: Focus = at < 5 ? [4.6, 21.2, 90.6, 15.6] : at < 9 ? [4.5, 70.5, 43, 14.4] : at < 11.5 ? [77.4, 87.6, 19.8, 6.5] : [42.8, 22.2, 23.6, 5.8];

  return (
    <Scene index="05" title={fr ? 'Configurez le volant' : 'Set up your wheel'} label={fr ? 'Matériel USB' : 'USB Hardware'}>
      <Columns>
        <div>
          {at < 5 ? <>
            <MainCopy>{fr ? <>Branchez le matériel. Cliquez sur <Accent>Actualiser</Accent>, puis choisissez le volant et les pédales.</> : <>Connect your hardware. Click <Accent>Refresh</Accent>, then choose your wheel and pedals.</>}</MainCopy>
            <Note>{fr ? 'Choisissez le périphérique DirectInput associé à chaque commande.' : 'Choose the DirectInput device for each control.'}</Note>
          </> : at < 9 ? <>
            <MainCopy>{fr ? <>Cliquez sur <Accent>Calibrer les axes</Accent> et suivez les consignes.</> : <>Click <Accent>Calibrate axes</Accent> and follow the prompts.</>}</MainCopy>
            <Note>{fr ? 'Capturez le centre et les butées du volant, puis le repos et la pleine course des pédales.' : 'Capture the center and full steering limits, then the pedal rest and fully pressed positions.'}</Note>
          </> : <>
            <MainCopy>{fr ? <><Accent>Enregistrer le profil</Accent>, puis <Accent>Activer l’USB en jeu</Accent>.</> : <><Accent>Save profile</Accent>, then <Accent>Enable USB in game</Accent>.</>}</MainCopy>
            <Note>{fr ? 'Vous pouvez fermer l’application après avoir enregistré et activé l’USB.' : 'Once the profile is saved and USB is enabled, you can close the app.'}</Note>
          </>}
        </div>
        <Evidence src={shot} maxHeight={541} focus={focus} caption={fr ? 'Interface réelle · aperçu sans matériel connecté' : 'Actual interface · preview without hardware connected'} />
      </Columns>
    </Scene>
  );
};

export const Simulation = ({locale}: {locale: Locale}) => {
  const fr = locale === 'fr';
  const shot = capture(`07-simulation-${locale}.png`) ?? asset(`simulation-${locale}.png`);

  return (
    <Scene index="06" title={fr ? 'Essayez sans volant' : 'Try it without a wheel'} label={fr ? 'Simulation clavier' : 'Keyboard simulation'}>
      <Columns>
        <div>
          <MainCopy>{fr ? <>Ouvrez <Accent>Simulation</Accent>, cliquez sur <Accent>Activer dans le jeu</Accent> et laissez l’application ouverte.</> : <>Open <Accent>Simulation</Accent>, click <Accent>Enable in game</Accent> and leave the app open.</>}</MainCopy>
          <Note>{fr ? 'Dans Cyberpunk 2077, installez V au volant. Relâchez d’abord les touches, puis essayez les commandes affichées.' : 'In Cyberpunk 2077, put V in the driver’s seat. Release the keys first, then try the controls shown.'}</Note>
        </div>
        <Evidence src={shot} maxHeight={541} focus={[72.4, 24.9, 22.6, 6.7]} caption={fr ? 'Interface réelle · mode aperçu' : 'Actual interface · preview mode'} />
      </Columns>
    </Scene>
  );
};

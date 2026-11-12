import {useEffect, useState, type ReactNode} from 'react';
import {
  AbsoluteFill, CanvasImage, Easing, getStaticFiles, interpolate, staticFile,
  useCurrentFrame, useDelayRender, useVideoConfig,
} from 'remotion';

export type Locale = 'fr' | 'en';
export type Focus = readonly [left: number, top: number, width: number, height: number];

export const palette = {black: '#080b0f', panel: '#121a21', yellow: '#fcee0a', cyan: '#00f0ff', text: '#f3f7fa', muted: '#abbcc8', line: '#33414b'};
export const heading = 'Barlow Condensed, sans-serif';
export const body = 'Barlow Semi Condensed, sans-serif';

export const asset = (name: string) => staticFile(`brand/${name}`);

export const capture = (name: string) => {
  const path = `captures/${name}`;
  return getStaticFiles().some((file) => file.name === path) ? staticFile(path) : undefined;
};

export const Logo = ({size = 53}: {size?: number}) => {
  const {fps} = useVideoConfig();
  return <CanvasImage src={asset('hud.png')} premountFor={fps} style={{width: size, height: size, flexShrink: 0}} />;
};

export const Scene = ({index, title, label, children}: {index: string; title: string; label: string; children: ReactNode}) => {
  const frame = useCurrentFrame();
  const {fps} = useVideoConfig();

  return (
    <AbsoluteFill style={{background: 'radial-gradient(ellipse at 80% 65%, #17242c 0%, #0b1117 40%, #080b0f 72%)', color: palette.text, fontFamily: body}}>
      <div style={{position: 'absolute', inset: '39px 86px 46px', borderTop: `1px solid ${palette.line}`, borderBottom: `1px solid ${palette.line}`}}>
        <div style={{width: 136, height: 4, background: palette.yellow, position: 'absolute', top: -1, left: 0}} />
      </div>
      <div style={{position: 'absolute', top: 66, left: 86, right: 86, display: 'flex', justifyContent: 'space-between', alignItems: 'center'}}>
        <div style={{display: 'flex', alignItems: 'center', gap: 13, color: palette.cyan, fontFamily: heading, fontSize: 27, letterSpacing: 2.5, textTransform: 'uppercase'}}><span style={{color: palette.yellow}}>{index}</span><span style={{width: 22, height: 1, background: palette.line}} />{label}</div>
        <div style={{display: 'flex', alignItems: 'center', gap: 7, color: palette.muted, fontFamily: heading, fontSize: 24, letterSpacing: 1.8}}><Logo size={35} />CHROME AXIS</div>
      </div>
      <div style={{position: 'absolute', top: 103, left: 86, right: 86, fontFamily: heading, fontSize: 76, lineHeight: 1, translate: `0 ${interpolate(frame, [0, 0.32 * fps], [10, 0], {extrapolateRight: 'clamp', easing: Easing.out(Easing.cubic)})}px`}}>{title}</div>
      <div style={{position: 'absolute', left: 86, right: 86, top: 211, bottom: 76}}>{children}</div>
    </AbsoluteFill>
  );
};

export const MainCopy = ({children}: {children: ReactNode}) => (
  <div style={{fontSize: 45, lineHeight: 1.11, color: palette.text, marginBottom: 24}}>{children}</div>
);

export const Note = ({children}: {children: ReactNode}) => (
  <div style={{borderLeft: `4px solid ${palette.cyan}`, paddingLeft: 17, color: palette.muted, fontSize: 29, lineHeight: 1.22}}>{children}</div>
);

export const Accent = ({children}: {children: ReactNode}) => <span style={{color: palette.yellow}}>{children}</span>;

export const PathLabel = ({children}: {children: ReactNode}) => (
  <div style={{fontFamily: 'Consolas, monospace', color: palette.cyan, background: '#0c1d23', border: '1px solid #2e6a71', padding: '18px 20px', fontSize: 28, lineHeight: 1.25, whiteSpace: 'nowrap'}}>{children}</div>
);

export const Columns = ({children}: {children: ReactNode}) => (
  <div style={{height: '100%', display: 'grid', gridTemplateColumns: '438px 1fr', columnGap: 54, alignItems: 'center'}}>{children}</div>
);

type Dimensions = {width: number; height: number};

const useImageSize = (src: string): Dimensions => {
  const {delayRender, continueRender, cancelRender} = useDelayRender();
  const [size, setSize] = useState<Dimensions>({width: 1, height: 1});
  const [handle] = useState(() => delayRender(`Read image size for ${src}`));

  useEffect(() => {
    const image = new Image();
    image.onload = () => {
      setSize({width: image.naturalWidth, height: image.naturalHeight});
      continueRender(handle);
    };
    image.onerror = () => cancelRender(new Error(`Cannot load ${src}`));
    image.src = src;
    return () => { image.onload = null; image.onerror = null; };
  }, [src, handle, continueRender, cancelRender]);

  return size;
};

export const Evidence = ({src, caption, maxWidth = 930, maxHeight = 545, focus, zoom = 1}: {src: string; caption: string; maxWidth?: number; maxHeight?: number; focus?: Focus; zoom?: number}) => {
  const {fps} = useVideoConfig();
  const natural = useImageSize(src);
  const scale = Math.min(maxWidth / natural.width, maxHeight / natural.height);
  const width = Math.round(natural.width * scale);
  const height = Math.round(natural.height * scale);
  const frame = useCurrentFrame();

  return (
    <div style={{display: 'flex', flexDirection: 'column', alignItems: 'center', minWidth: 0}}>
      <div style={{width, height, borderRadius: 6, border: '1px solid #5b6b76', overflow: 'hidden', position: 'relative', boxShadow: '0 15px 60px #0009'}}>
        <div style={{width: '100%', height: '100%', position: 'relative', transformOrigin: focus ? `${focus[0] + focus[2] / 2}% ${focus[1] + focus[3] / 2}%` : 'center', scale: interpolate(frame, [0, 0.55 * fps], [1, zoom], {extrapolateRight: 'clamp', easing: Easing.out(Easing.cubic)})}}>
          <CanvasImage src={src} premountFor={fps} style={{width: '100%', height: '100%', display: 'block'}} />
          {focus ? <div style={{position: 'absolute', left: `${focus[0]}%`, top: `${focus[1]}%`, width: `${focus[2]}%`, height: `${focus[3]}%`, boxSizing: 'border-box', border: `3px solid ${palette.cyan}`, borderRadius: 5, boxShadow: '0 0 0 3000px #05090e24, 0 0 12px #00f0ff88'}} /> : null}
        </div>
      </div>
      <div style={{fontFamily: body, fontSize: 23, color: palette.muted, marginTop: 12, textAlign: 'center'}}>{caption}</div>
    </div>
  );
};

// Type contract for the passive receiver. app.js is the dependency-free browser build.
export type PassiveSignal =
  | { type: 'authenticate'; token: string; mode: 'passive'; role: 'receiver' }
  | { type: 'authenticated'; mode: 'passive'; role: 'receiver' }
  | { type: 'ready' }
  | { type: 'peer-left' }
  | { type: 'offer' | 'answer'; sdp: string; mode?: 'passive' }
  | { type: 'candidate'; candidate: RTCIceCandidateInit };

export type FrameMetric = {
  callbackTime: number;
  mediaTime: number;
  captureTime: number | null;
  receiveTime: number | null;
  presentationTime: number | null;
  expectedDisplayTime: number;
  processingDuration: number | null;
  presentedFrames: number;
};

export const PASSIVE_MODE = Object.freeze({
  inputAvailable: false,
  acceptedDataChannels: [] as string[],
  macros: false,
  automation: false,
});

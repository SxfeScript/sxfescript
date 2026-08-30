// Type-only declarations: none of these has a runtime representation.
type Id = number;
export type Name = string;
interface Local { a: number }
export interface Point { readonly x: number; y?: string }
declare const injected: number;
export type { Id };

// Annotations, optional parameters, generics, as/satisfies.
export const scale = (p: Point, k: number): Point => ({ x: p.x * k, y: p.y });
export function pick<T>(a: T, b?: T): T { return b === undefined ? a : b; }
export function pair<A, B>(a: A, b: B): [A, B] { return [a, b]; }
export const widened = (1 as unknown) as number;
export const checked = { a: 1 } satisfies { a: number };
export const union = 5 as number | string;
export const arithmetic = (1 as number) + 2;
export const compared = 1 < 2;
const l: Local = { a: 3 };
export const localUse: number = l.a;

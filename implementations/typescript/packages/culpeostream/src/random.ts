export interface RandomSource {
  fill(target: Uint8Array): Uint8Array;
}

export const defaultRandomSource: RandomSource = {
  fill(target: Uint8Array): Uint8Array {
    const cryptoObject = globalThis.crypto;
    if (!cryptoObject?.getRandomValues) {
      throw new Error("Secure randomness is unavailable in this runtime.");
    }
    return cryptoObject.getRandomValues(target);
  },
};

export function createRandomId(
  byteLength = 16,
  source: RandomSource = defaultRandomSource,
): string {
  const bytes = source.fill(new Uint8Array(byteLength));
  return Array.from(bytes, (value) => value.toString(16).padStart(2, "0")).join(
    "",
  );
}

import type { CloseCode, InitErrorCode } from "./types.js";

export class CulpeoError extends Error {
  public readonly code: CloseCode | InitErrorCode;

  public constructor(code: CloseCode | InitErrorCode, message: string) {
    super(message);
    this.name = "CulpeoError";
    this.code = code;
  }
}

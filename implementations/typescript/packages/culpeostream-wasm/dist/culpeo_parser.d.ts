/**
 * Type declaration for the Emscripten-compiled WASM glue module.
 *
 * The shape matches both the real compiled output and the stub file
 * committed for CI environments (see dist/culpeo_parser.js).
 */

export interface CulpeoParserModule {
  /** Sentinel: present and true only in the stub file. */
  __culpeoStub?: boolean;

  _malloc(size: number): number;
  _free(ptr: number): void;

  _culpeo_parse_headers(
    buf: number,
    len: number,
    headersOut: number,
    maxHeaders: number,
    bodyOffsetOut: number,
  ): number;

  _culpeo_serialize_frame(
    headers: number,
    headerCount: number,
    stringsBuf: number,
    body: number,
    bodyLen: number,
    outBuf: number,
    outCap: number,
  ): number;

  HEAPU8: Uint8Array;
  getValue(ptr: number, type: string): number;
  setValue(ptr: number, value: number, type: string): void;
}

export type CreateModuleOptions = {
  locateFile?: (path: string) => string;
};

export declare function createCulpeoParserModule(
  options?: CreateModuleOptions,
): Promise<CulpeoParserModule>;

export default createCulpeoParserModule;

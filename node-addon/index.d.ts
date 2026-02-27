/** Request for buildTransactionHex. All numeric fields accept number or hex string. */
interface BuildTransactionRequest {
  fromWalletId: number | string;
  toWalletId: number | string;
  amount: number | string;
  fee?: number | string;
  type?: number | string;
  tokenId?: number | string;
  metaHex?: string;
  idempotentId?: number | string;
  validationTsMin?: number | string;
  validationTsMax?: number | string;
}

interface AddTransactionRequest {
  transactionHex: string;
  signaturesHex: string[];
}

interface FetchTransactionsByWalletRequest {
  walletId: number | string;
  beforeBlockId?: number | string;
}

interface FetchTransactionByIndexRequest {
  txIndex: number | string;
}

declare class Client {
  constructor(endpoint?: string);

  setEndpoint(endpoint: string): void;

  fetchBeaconState(): Promise<Record<string, unknown>>;
  fetchCalibration(): Promise<Record<string, unknown>>;
  fetchMinerList(): Promise<Record<string, unknown>[]>;
  fetchMinerStatus(): Promise<Record<string, unknown>>;
  fetchBlock(blockId: number | string): Promise<Record<string, unknown>>;
  fetchUserAccount(accountId: number | string): Promise<Record<string, unknown>>;
  fetchTransactionsByWallet(request: FetchTransactionsByWalletRequest): Promise<Record<string, unknown>>;
  fetchTransactionByIndex(request: FetchTransactionByIndexRequest): Promise<Record<string, unknown>>;

  buildTransactionHex(request: BuildTransactionRequest): string;
  addTransaction(request: AddTransactionRequest): Promise<boolean>;
}

declare const addon: { Client: typeof Client };
export type {
  BuildTransactionRequest,
  AddTransactionRequest,
  FetchTransactionsByWalletRequest,
  FetchTransactionByIndexRequest,
};
export = addon;

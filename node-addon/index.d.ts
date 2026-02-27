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

  fetchBeaconState(): Promise<object>;
  fetchCalibration(): Promise<object>;
  fetchMinerList(): Promise<object>;
  fetchMinerStatus(): Promise<object>;
  fetchBlock(blockId: number | string): Promise<object>;
  fetchUserAccount(accountId: number | string): Promise<object>;
  fetchTransactionsByWallet(request: FetchTransactionsByWalletRequest): Promise<object>;
  fetchTransactionByIndex(request: FetchTransactionByIndexRequest): Promise<object>;

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

// AsyncStorage has no native module under Jest; use the library's official mock so any module
// that persists to AsyncStorage (src/lib/store.ts) doesn't throw at import/use time.
jest.mock('@react-native-async-storage/async-storage', () =>
  require('@react-native-async-storage/async-storage/jest/async-storage-mock'),
)

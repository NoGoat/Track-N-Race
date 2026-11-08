import Store from 'electron-store'

// One main-process store instance is shared by IPC writers and native-engine
// subscribers. Separate Store instances point at the same file but do not form
// a reliable in-process publish/subscribe channel, which made recording and
// output-directory changes appear to require an app restart.
export const configStore = new Store()

// True when this launch found no saved settings at all, i.e. a fresh install.
// Read once at module load, before anything writes to the store.
export const configWasEmpty = configStore.size === 0

import { useSyncExternalStore } from 'react'
import { store } from './store.js'

// Подписка на стор; возвращает его же (данные читаем через геттеры).
export function useStore() {
  useSyncExternalStore(store.subscribe, store.getVersion)
  return store
}

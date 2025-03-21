module Worker () where

import Foreign.StablePtr (StablePtr, newStablePtr)
import Goal (Goal (Goal))

foreign export ccall "nix_libstore_init_worker_state" nix_libstore_init_worker_state :: IO (StablePtr Goal)

init :: Goal
init = Goal

nix_libstore_init_worker_state = do
  putStrLn "HELLO WORLD"
  let !() = error "Hello"
  newStablePtr Goal

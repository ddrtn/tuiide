_tuiide()
{
  local current previous
  COMPREPLY=()
  current="${COMP_WORDS[COMP_CWORD]}"
  previous="${COMP_WORDS[COMP_CWORD-1]}"

  case "${previous}" in
    --project)
      mapfile -t COMPREPLY < <(compgen -d -- "${current}")
      return
      ;;
    --log-file)
      mapfile -t COMPREPLY < <(compgen -f -- "${current}")
      return
      ;;
  esac

  if [[ "${current}" == -* ]]; then
    mapfile -t COMPREPLY < <(compgen -W '--help --version --project --log-file --diagnostic' -- "${current}")
  else
    mapfile -t COMPREPLY < <(compgen -d -- "${current}")
  fi
}
complete -F _tuiide tuiide

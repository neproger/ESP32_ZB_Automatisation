// Переиспользуемый свитч (свой CSS-класс .switch, без UI-библиотек).
export default function Switch({ checked, onChange, disabled = false }) {
  return (
    <label className="switch">
      <input
        type="checkbox"
        checked={checked}
        disabled={disabled}
        onChange={(e) => onChange?.(e.target.checked)}
      />
      <span />
    </label>
  )
}

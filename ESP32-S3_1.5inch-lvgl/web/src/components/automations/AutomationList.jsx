export default function AutomationList({ sorted, onEdit, onToggleEnabled, onRemove }) {
	return (
		<div className="card">
			<div className="table-wrap">
				<table>
					<thead>
						<tr>
							<th>id</th>
							<th>name</th>
							<th>enabled</th>
							<th />
						</tr>
					</thead>
					<tbody>
						{sorted.length === 0 ? (
							<tr>
								<td colSpan={4} className="muted">
									No automations yet.
								</td>
							</tr>
						) : (
							sorted.map((a) => (
								<tr key={String(a?.id ?? Math.random())}>
									<td className="mono">{String(a?.id ?? '')}</td>
									<td>{String(a?.name ?? '')}</td>
									<td className="muted">{Boolean(a?.enabled) ? 'true' : 'false'}</td>
									<td className="row">
										<button onClick={() => onEdit(a)}>Edit</button>
										<button onClick={() => onToggleEnabled(a)}>{Boolean(a?.enabled) ? 'Disable' : 'Enable'}</button>
										<button onClick={() => onRemove(a)}>Remove</button>
									</td>
								</tr>
							))
						)}
					</tbody>
				</table>
			</div>
		</div>
	)
}

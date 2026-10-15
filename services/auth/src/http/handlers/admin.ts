import type { Request, Response } from 'express';
import type { AdminService } from '../../services/admin.js';
import { authOf, parse } from '../middleware.js';
import { banBody, idParams, roleBody } from '../schemas.js';

export function adminHandlers(admin: AdminService) {
  return {
    async ban(req: Request, res: Response) {
      const { id } = parse(idParams, req.params);
      res.status(201).json(await admin.ban(authOf(res).accountId, id, parse(banBody, req.body)));
    },

    async unban(req: Request, res: Response) {
      await admin.unban(authOf(res).accountId, parse(idParams, req.params).id);
      res.status(204).end();
    },

    async setRole(req: Request, res: Response) {
      const { id } = parse(idParams, req.params);
      res.json(await admin.setRole(authOf(res).accountId, id, parse(roleBody, req.body).role));
    },
  };
}

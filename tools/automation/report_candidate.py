"""MIT. Trusted main-branch notification code, never code from a candidate."""
import base64
import json
import os
import re
from upstream_sync import GitHub

def main():
    sha = os.environ['CANDIDATE_SHA']
    if not re.fullmatch(r'[0-9a-f]{40}', sha):
        raise ValueError('Invalid source SHA')
    api = GitHub(os.environ['GITHUB_REPOSITORY'], os.environ['GITHUB_TOKEN'])
    data = api.call('contents/release.json?ref=' + sha)
    tag = json.loads(base64.b64decode(data['content']))['upstream_tag']
    if not re.fullmatch(r'v\d+\.\d+\.\d+', tag): raise ValueError('Invalid upstream version')
    title = 'Upstream ' + tag + ': PowerfulMoon review'
    issue = next((i for i in api.call('issues?state=all&per_page=100') if i['title'] == title and 'pull_request' not in i), None)
    url = os.environ['GITHUB_SERVER_URL'] + '/' + api.repo + '/actions/runs/' + os.environ['GITHUB_RUN_ID']
    result = os.environ['BUILD_RESULT']
    body = ('Candidate `' + sha + '`\n\nBuild/test: **' + result + '**. [Open run and artifacts](' + url + ').\n\n'
            'Nothing was published. ' + ('Download the candidate artifact and test before owner approval.' if result == 'success' else 'Repair the failure and create a new passing candidate before approval.'))
    if issue:
        api.call('issues/' + str(issue['number']) + '/comments', {'body': body})
    else:
        api.call('issues', {'title': title, 'body': body})

if __name__ == '__main__': main()
